/**
 * LLM アダプタ抽象層。
 * Phase 0 の「モック差し替え可能」要件 (spec §3) を満たすための注入インタフェース。
 * AnthropicAdapter が本番実装、MockAdapter がテスト用。
 */

import type {
  EconomyPhase0,
  MechanicMid,
  MacroEvaluation,
  MechanicEvaluation,
  CombinationCandidate,
  IncoherenceIssue,
  Step5Output,
} from "./types.js";

// ── インタフェース ────────────────────────────────────────────────────────────

export interface LlmAdapter {
  /** ③ resource グラフ生成: mechanic 群 → produce/consume エッジ推論 */
  generateEconomyGraph(
    slug: string,
    mechanics: MechanicMid[],
  ): Promise<EconomyPhase0>;

  /** ④ ミクロ評価: 個々のメカニクス評価 */
  evaluateMechanics(
    mechanics: MechanicMid[],
    genre: string,
    preferenceScore: number,
  ): Promise<MechanicEvaluation[]>;

  /** ④ マクロ評価: ゲーム全体構造評価 */
  evaluateMacro(
    slug: string,
    mechanics: MechanicMid[],
    economy: EconomyPhase0,
  ): Promise<MacroEvaluation>;

  /** ⑤ コア/オプション分解 */
  decomposeCoreOption(
    mechanics: MechanicMid[],
    economy: EconomyPhase0,
  ): Promise<Step5Output>;

  /** ⑦ 破綻検出 */
  detectIncoherence(
    candidates: CombinationCandidate[],
    economy: EconomyPhase0,
  ): Promise<IncoherenceIssue[]>;
}

// ── Anthropic 本番実装 ────────────────────────────────────────────────────────

const ANTHROPIC_MODEL = "claude-haiku-4-5-20251001";
const TIMEOUT_MS = 90_000;

async function callAnthropic(
  apiKey: string,
  prompt: string,
  systemPrompt: string,
): Promise<string> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), TIMEOUT_MS);
  try {
    const res = await fetch("https://api.anthropic.com/v1/messages", {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-api-key": apiKey,
        "anthropic-version": "2023-06-01",
      },
      body: JSON.stringify({
        model: ANTHROPIC_MODEL,
        max_tokens: 2048,
        system: systemPrompt,
        messages: [{ role: "user", content: prompt }],
      }),
      signal: controller.signal,
    });
    if (!res.ok) throw new Error(`anthropic http ${res.status}: ${await res.text()}`);
    const json = await res.json() as { content: Array<{ type: string; text: string }> };
    return json.content.filter((c) => c.type === "text").map((c) => c.text).join("").trim();
  } finally {
    clearTimeout(timer);
  }
}

function extractJson(text: string): unknown {
  const fence = /```(?:json)?\s*([\s\S]*?)```/.exec(text);
  const raw = fence ? fence[1].trim() : (() => {
    const s = text.indexOf("{");
    const e = text.lastIndexOf("}");
    return s !== -1 && e !== -1 ? text.slice(s, e + 1) : text.trim();
  })();
  return JSON.parse(raw);
}

export function createAnthropicAdapter(apiKey: string): LlmAdapter {
  return {
    async generateEconomyGraph(slug, mechanics) {
      const mechanicsJson = JSON.stringify(mechanics.map((m) => ({ name: m.name, description: m.description, intends: m.intends })));
      const text = await callAnthropic(
        apiKey,
        `Game slug: ${slug}\nMechanics: ${mechanicsJson}\n\nOutput JSON only.`,
        `You are a game design analyst. Given a list of mechanics, infer the resource flow (produce/consume) between mechanics and abstract resources.

Output a single JSON object:
{
  "resources": [{"name": "string", "description": "string"}],
  "edges": [{"sourceMechanic": "mechanic name", "targetResource": "resource name", "type": "produces|consumes"}]
}

Keep it focused: 3-8 resources, 5-15 edges. No markdown. JSON only.`,
      );
      const parsed = extractJson(text) as { resources: Array<{ name: string; description?: string }>; edges: Array<{ sourceMechanic: string; targetResource: string; type: "produces" | "consumes" }> };
      return {
        mechanics,
        resources: parsed.resources ?? [],
        edges: parsed.edges ?? [],
        cached: false,
      };
    },

    async evaluateMechanics(mechanics, genre, preferenceScore) {
      const mechanicsJson = JSON.stringify(mechanics.map((m) => ({ id: m.id, name: m.name, description: m.description })));
      const text = await callAnthropic(
        apiKey,
        `Genre: ${genre}\nPreference compatibility baseline: ${preferenceScore.toFixed(2)}\nMechanics: ${mechanicsJson}\n\nOutput JSON only.`,
        `Evaluate each mechanic individually (micro scale). For each mechanic output:
- score (0..1, higher = better design quality)
- rationale (1 sentence)
- preferenceCompatibility (0..1, how well this mechanic fits the genre's known audience preferences)
- isNegative (true if score < 0.4)

Output: { "evaluations": [{"mechanicId": "...", "mechanicName": "...", "scale": "micro", "score": 0.0, "rationale": "...", "preferenceCompatibility": 0.0, "isNegative": false}] }
JSON only.`,
      );
      const parsed = extractJson(text) as { evaluations: MechanicEvaluation[] };
      return parsed.evaluations ?? [];
    },

    async evaluateMacro(slug, mechanics, economy) {
      const input = JSON.stringify({ slug, mechanicCount: mechanics.length, resources: economy.resources, edgeCount: economy.edges.length });
      const text = await callAnthropic(
        apiKey,
        `Game data: ${input}\n\nOutput JSON only.`,
        `Evaluate the game's overall macro structure. Produce:
{
  "score": 0.0,
  "rationale": "2-3 sentences",
  "economyBalance": "balanced|inflationary|deflationary|deadend|unknown",
  "progressionCurveEstimated": true,
  "progressionCurveSummary": "1-2 sentences about the estimated progression arc",
  "mechanicInteractions": [{"a": "mechanic name", "b": "mechanic name", "type": "synergy|cannibalize|isolated"}]
}
JSON only.`,
      );
      const parsed = extractJson(text) as MacroEvaluation;
      return { ...parsed, progressionCurveEstimated: true as const };
    },

    async decomposeCoreOption(mechanics, economy) {
      const input = JSON.stringify({ mechanics: mechanics.map((m) => m.name), edges: economy.edges });
      const text = await callAnthropic(
        apiKey,
        `Mechanics and economy: ${input}\n\nOutput JSON only.`,
        `Classify each mechanic as "core" or "option".
Core = essential to the primary game loop, high economy centrality, removal breaks the game.
Option = supplementary, enriches but not necessary.

Output:
{
  "roles": [{"mechanicId": "...", "mechanicName": "...", "role": "core|option", "rationale": "1 sentence", "economyCentrality": 0.0}]
}
JSON only.`,
      );
      const parsed = extractJson(text) as { roles: Step5Output["roles"] };
      const roles = parsed.roles ?? [];
      return {
        roles,
        coreMechanics: roles.filter((r) => r.role === "core").map((r) => r.mechanicId),
        optionMechanics: roles.filter((r) => r.role === "option").map((r) => r.mechanicId),
      };
    },

    async detectIncoherence(candidates, economy) {
      const input = JSON.stringify({ candidates: candidates.map((c) => ({ mechanic: c.mechanic.name, sourceGame: c.sourceGame, playContextCompatible: c.playContextCompatible })), edgeCount: economy.edges.length });
      const text = await callAnthropic(
        apiKey,
        `Combination candidates and current economy: ${input}\n\nOutput JSON only.`,
        `Detect incoherence issues from combining these mechanics into the existing game.
Issue types: economy_imbalance | progression_monotone | loop_dead_end | preference_mismatch | mechanic_conflict

Output:
{
  "issues": [{"type": "...", "description": "1 sentence", "affectedMechanics": ["name"], "suggestedFix": "1 sentence"}]
}
If no issues found, return {"issues": []}. JSON only.`,
      );
      const parsed = extractJson(text) as { issues: IncoherenceIssue[] };
      return parsed.issues ?? [];
    },
  };
}

// ── モック実装 (Phase 0 テスト用) ─────────────────────────────────────────────

export function createMockAdapter(): LlmAdapter {
  return {
    async generateEconomyGraph(_slug, mechanics) {
      const resources = [
        { name: "experience_points", description: "Progress currency" },
        { name: "health", description: "Attrition resource" },
        { name: "score", description: "Win condition metric" },
      ];
      const edges = mechanics.slice(0, 3).flatMap((m, i) => [
        { sourceMechanic: m.name, targetResource: resources[i % resources.length].name, type: "produces" as const },
        { sourceMechanic: m.name, targetResource: resources[(i + 1) % resources.length].name, type: "consumes" as const },
      ]);
      return { mechanics, resources, edges, cached: false };
    },

    async evaluateMechanics(mechanics, _genre, preferenceScore) {
      return mechanics.map((m, i) => ({
        mechanicId: m.id,
        mechanicName: m.name,
        scale: "micro" as const,
        score: 0.5 + (i % 3) * 0.15,
        rationale: `Mock evaluation for ${m.name}`,
        preferenceCompatibility: preferenceScore,
        isNegative: false,
      }));
    },

    async evaluateMacro(slug, _mechanics, _economy) {
      return {
        score: 0.7,
        rationale: `Mock macro evaluation for ${slug}. Economy appears balanced.`,
        economyBalance: "balanced" as const,
        progressionCurveEstimated: true as const,
        progressionCurveSummary: "Mock: steady escalation with tension peaks at mid and late game.",
        mechanicInteractions: [],
      };
    },

    async decomposeCoreOption(mechanics, _economy) {
      const roles = mechanics.map((m, i) => ({
        mechanicId: m.id,
        mechanicName: m.name,
        role: (i < Math.ceil(mechanics.length / 2) ? "core" : "option") as "core" | "option",
        rationale: `Mock: ${i < Math.ceil(mechanics.length / 2) ? "primary loop" : "supplementary"} mechanic`,
        economyCentrality: i < Math.ceil(mechanics.length / 2) ? 0.8 : 0.3,
      }));
      return {
        roles,
        coreMechanics: roles.filter((r) => r.role === "core").map((r) => r.mechanicId),
        optionMechanics: roles.filter((r) => r.role === "option").map((r) => r.mechanicId),
      };
    },

    async detectIncoherence(_candidates, _economy) {
      return [];
    },
  };
}
