/**
 * LLM-powered mechanics decomposition analyzer with DB tool use.
 *
 * The LLM can call two DB tools to gather information before producing the graph:
 *   - query_guide_mechanics: ludus RDBMS (ludusMechanics table)
 *   - search_discussion_knowledge: Core KG SQLite (utterances full-text search)
 *
 * Mechanic granularity is deliberately coarse:
 *   engine / attrition / win_condition / resource / experience
 * — enough to show resource relationships and the primary gameplay loop.
 */

import { randomUUID } from "node:crypto";
import { ludusMechanicRepo, economyGraphRepo } from "../db/repository.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { getConfig } from "../config.js";

export interface GraphNode {
  id: string;
  type: "engine" | "attrition" | "win_condition" | "resource" | "experience";
  label: string;
  description: string;
}

export interface GraphEdge {
  id: string;
  source: string;
  target: string;
  type:
    | "produces"
    | "consumes"
    | "enables"
    | "requires"
    | "drives"
    | "suppresses"
    | "leads_to"
    | "reinforces"
    | "feedback";
  label: string;
}

export interface EconomyGraph {
  gameTitle: string;
  nodes: GraphNode[];
  edges: GraphEdge[];
  summary: string;
  economyLoops: string[];
  analyzedAt: string;
  mechanicCount: number;
}

const ANALYSIS_MODEL = "claude-haiku-4-5-20251001";
const ANALYSIS_MAX_TOKENS = 4096;
const ANALYSIS_TIMEOUT_MS = 120_000;
const MAX_TOOL_TURNS = 4;

// ── Tool definitions for Anthropic tool use ──────────────────────────────────

const TOOLS = [
  {
    name: "query_guide_mechanics",
    description:
      "Query mechanics detected from guide articles for a specific game from the ludus database (RDBMS). Returns mechanic names and their confidence scores extracted from RSS/web sources.",
    input_schema: {
      type: "object" as const,
      properties: {
        game_title: { type: "string", description: "Exact or approximate game title to search for" },
        limit: { type: "integer", description: "Maximum number of results to return (default 30)", default: 30 },
      },
      required: ["game_title"],
    },
  },
  {
    name: "search_discussion_knowledge",
    description:
      "Search utterances in the Core Knowledge Graph (SQLite) for a keyword. Returns relevant quotes from discussions about game design. Use this to find player observations, design gap insights, and hypothesis text related to mechanics.",
    input_schema: {
      type: "object" as const,
      properties: {
        keyword: { type: "string", description: "Keyword or mechanic term to search for in discussion utterances" },
        limit: { type: "integer", description: "Maximum number of results to return (default 10)", default: 10 },
      },
      required: ["keyword"],
    },
  },
];

// ── Tool execution ────────────────────────────────────────────────────────────

async function executeQueryGuideMechanics(
  workspaceId: string,
  input: { game_title: string; limit?: number },
): Promise<string> {
  const rows = await ludusMechanicRepo.searchByGame(workspaceId, input.game_title);
  const limited = rows.slice(0, Math.min(input.limit ?? 30, 50));
  if (limited.length === 0) {
    return `No mechanics found in guide data for "${input.game_title}". Try the exact game title or a variant spelling.`;
  }
  const lines = limited.map((m) => `- ${m.mechanic} (confidence: ${m.confidence}%, source: ${m.sourceTitle ?? m.sourceUrl})`);
  return `Guide mechanics for "${input.game_title}" (${limited.length} results):\n${lines.join("\n")}`;
}

function executeSearchDiscussionKnowledge(
  workspaceId: string,
  input: { keyword: string; limit?: number },
): string {
  const limit = Math.min(input.limit ?? 10, 20);
  try {
    const core = createCore(resolveActiveKgPath(getConfig()));
    try {
      const rows = core.client.raw
        .prepare(
          `SELECT u.normalized_content AS content, s.title AS session_title
           FROM utterances u
           JOIN sessions s ON s.id = u.session_id
           WHERE u.workspace_id = ?
             AND (u.normalized_content LIKE ? OR u.raw_content LIKE ?)
           ORDER BY u.created_at DESC
           LIMIT ?`,
        )
        .all(workspaceId, `%${input.keyword}%`, `%${input.keyword}%`, limit) as Array<{
        content: string;
        session_title: string;
      }>;

      if (rows.length === 0) {
        return `No discussion utterances found containing "${input.keyword}".`;
      }
      const lines = rows.map((r) => `[${r.session_title ?? "?"}] ${(r.content ?? "").slice(0, 200)}`);
      return `Discussion knowledge for "${input.keyword}" (${rows.length} results):\n${lines.join("\n")}`;
    } finally {
      core.close();
    }
  } catch {
    return `Discussion knowledge search unavailable (KG not initialized or empty).`;
  }
}

// ── Anthropic tool-use loop ───────────────────────────────────────────────────

type AnthropicContent =
  | { type: "text"; text: string }
  | { type: "tool_use"; id: string; name: string; input: Record<string, unknown> }
  | { type: "tool_result"; tool_use_id: string; content: string };

interface AnthropicResponse {
  stop_reason: string;
  content: AnthropicContent[];
  usage?: { input_tokens?: number; output_tokens?: number };
}

type Message = { role: "user" | "assistant"; content: string | AnthropicContent[] };

async function runToolLoop(
  systemPrompt: string,
  userMessage: string,
  workspaceId: string,
  apiKey: string,
): Promise<{ ok: true; text: string } | { ok: false; error: string }> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), ANALYSIS_TIMEOUT_MS);

  const messages: Message[] = [{ role: "user", content: userMessage }];

  try {
    for (let turn = 0; turn <= MAX_TOOL_TURNS; turn++) {
      const res = await fetch("https://api.anthropic.com/v1/messages", {
        method: "POST",
        headers: {
          "content-type": "application/json",
          "x-api-key": apiKey,
          "anthropic-version": "2023-06-01",
        },
        body: JSON.stringify({
          model: ANALYSIS_MODEL,
          max_tokens: ANALYSIS_MAX_TOKENS,
          system: systemPrompt,
          tools: TOOLS,
          messages,
        }),
        signal: controller.signal,
      });

      if (!res.ok) {
        const body = await res.text().catch(() => "");
        return { ok: false, error: `anthropic http ${res.status}: ${body.slice(0, 200)}` };
      }

      const json = (await res.json()) as AnthropicResponse;

      if (json.stop_reason === "end_turn") {
        const text = json.content
          .filter((c): c is { type: "text"; text: string } => c.type === "text")
          .map((c) => c.text)
          .join("\n")
          .trim();
        if (!text) return { ok: false, error: "empty response" };
        return { ok: true, text };
      }

      if (json.stop_reason !== "tool_use") {
        const text = json.content
          .filter((c): c is { type: "text"; text: string } => c.type === "text")
          .map((c) => c.text)
          .join("\n")
          .trim();
        if (text) return { ok: true, text };
        return { ok: false, error: `unexpected stop_reason: ${json.stop_reason}` };
      }

      if (turn >= MAX_TOOL_TURNS) {
        return { ok: false, error: "tool loop limit reached" };
      }

      messages.push({ role: "assistant", content: json.content });

      const toolResults: Array<{ type: "tool_result"; tool_use_id: string; content: string }> = [];
      for (const block of json.content) {
        if (block.type !== "tool_use") continue;
        let result: string;
        if (block.name === "query_guide_mechanics") {
          result = await executeQueryGuideMechanics(workspaceId, block.input as { game_title: string; limit?: number });
        } else if (block.name === "search_discussion_knowledge") {
          result = executeSearchDiscussionKnowledge(workspaceId, block.input as { keyword: string; limit?: number });
        } else {
          result = `Unknown tool: ${block.name}`;
        }
        console.log(`[economy-analyzer] tool=${block.name} → ${result.slice(0, 80)}...`);
        toolResults.push({ type: "tool_result", tool_use_id: block.id, content: result });
      }

      messages.push({ role: "user", content: toolResults });
    }

    return { ok: false, error: "tool loop exhausted without final response" };
  } catch (err) {
    return { ok: false, error: `llm call failed: ${(err as Error).message}` };
  } finally {
    clearTimeout(timer);
  }
}

// ── Prompt ───────────────────────────────────────────────────────────────────

const SYSTEM_PROMPT = `You are a game design analyst specializing in internal economy and systems design.

Your task: analyze a game's mechanics and produce a structured graph that shows resource relationships and gameplay loops.

You have access to two database tools:
1. query_guide_mechanics — queries mechanics data extracted from game guides and articles
2. search_discussion_knowledge — searches player/designer discussion insights from the knowledge graph

Use these tools to gather information about the game before producing the final graph. Query for the main mechanics first, then search for specific concepts you need to understand better.

MECHANIC GRANULARITY — use these coarse categories for nodes (not fine-grained systems):
- engine: A core mechanism that drives the game's primary loop (e.g., "deckbuilding engine", "wave spawn engine", "income loop")
- attrition: A depletion/wear system where resources diminish over time or through play (e.g., "HP attrition", "deck thinning", "time pressure")
- win_condition: The goal state or victory/failure condition (e.g., "boss defeat", "score threshold", "survival time")
- resource: A tracked quantity that flows through the game (e.g., "gold", "cards-in-hand", "energy", "upgrade points")
- experience: A player emotional/cognitive state created by the system (e.g., "escalating tension", "build satisfaction", "decision anxiety")

EDGE TYPES:
- produces: X generates Y
- consumes: X uses up Y
- enables: X makes Y possible/easier
- requires: X cannot work without Y
- drives: X pushes Y into action
- suppresses: X reduces Y's effectiveness
- leads_to: X creates player experience Y
- reinforces: positive feedback — X creates more X (through Y)
- feedback: X's output returns as X's input

OUTPUT: A single JSON object (no markdown, no explanation):
{
  "nodes": [{ "id": "kebab-id", "type": "engine|attrition|win_condition|resource|experience", "label": "Short Name", "description": "1-2 sentences" }],
  "edges": [{ "id": "e1", "source": "node-id", "target": "node-id", "type": "produces|consumes|...", "label": "short verb" }],
  "summary": "2-3 sentences describing the core economy loop and primary gameplay experience.",
  "economyLoops": ["Description of each major feedback loop"]
}

Keep it focused: 6-15 nodes, 8-20 edges. Every node must be in at least one edge. Output only JSON.`;

function buildUserMessage(gameTitle: string): string {
  return `Analyze the internal economy and resource relationships of: **${gameTitle}**

Use the DB tools to gather mechanics data, then produce the graph JSON.
Focus on: what resources exist, how the engine creates/consumes them, what leads to attrition, and what the win condition demands.`;
}

// ── JSON extraction ───────────────────────────────────────────────────────────

function extractJson(text: string): string {
  const fenceMatch = /```(?:json)?\s*([\s\S]*?)```/.exec(text);
  if (fenceMatch) return fenceMatch[1].trim();
  const braceStart = text.indexOf("{");
  const braceEnd = text.lastIndexOf("}");
  if (braceStart !== -1 && braceEnd !== -1) return text.slice(braceStart, braceEnd + 1);
  return text.trim();
}

export function toSlug(gameTitle: string): string {
  return gameTitle.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");
}

// ── Public API ────────────────────────────────────────────────────────────────

export async function analyzeEconomy(
  workspaceId: string,
  gameTitle: string,
  apiKey: string,
  opts?: { force?: boolean },
): Promise<{ ok: true; graph: EconomyGraph; cached: boolean } | { ok: false; error: string }> {
  console.log(`[economy-analyzer] start: "${gameTitle}" ws=${workspaceId}`);

  if (!opts?.force) {
    const cached = await economyGraphRepo.findByGame(workspaceId, gameTitle);
    if (cached) {
      console.log(`[economy-analyzer] cache hit: "${gameTitle}"`);
      return { ok: true, graph: JSON.parse(cached.graphJson) as EconomyGraph, cached: true };
    }
  }

  if (!apiKey) {
    return { ok: false, error: "ANTHROPIC_API_KEY not set" };
  }

  const mechanics = await ludusMechanicRepo.searchByGame(workspaceId, gameTitle);
  console.log(`[economy-analyzer] calling LLM (tool-use) for "${gameTitle}" (${mechanics.length} guide mechanics available)`);

  const result = await runToolLoop(SYSTEM_PROMPT, buildUserMessage(gameTitle), workspaceId, apiKey);

  if (!result.ok) {
    console.error(`[economy-analyzer] LLM error: ${result.error}`);
    return { ok: false, error: result.error };
  }

  let parsed: { nodes: GraphNode[]; edges: GraphEdge[]; summary: string; economyLoops: string[] };
  try {
    parsed = JSON.parse(extractJson(result.text));
  } catch (err) {
    console.error(`[economy-analyzer] JSON parse failed: ${(err as Error).message}\nraw:\n${result.text.slice(0, 500)}`);
    return { ok: false, error: `JSON parse failed: ${(err as Error).message}` };
  }

  const graph: EconomyGraph = {
    gameTitle,
    nodes: parsed.nodes ?? [],
    edges: parsed.edges ?? [],
    summary: parsed.summary ?? "",
    economyLoops: parsed.economyLoops ?? [],
    analyzedAt: new Date().toISOString(),
    mechanicCount: mechanics.length,
  };

  await economyGraphRepo.upsert({
    id: randomUUID(),
    workspaceId,
    gameTitle,
    graphJson: JSON.stringify(graph),
    mechanicCount: mechanics.length,
  });

  console.log(
    `[economy-analyzer] done: "${gameTitle}" nodes=${graph.nodes.length} edges=${graph.edges.length}`,
  );

  return { ok: true, graph, cached: false };
}
