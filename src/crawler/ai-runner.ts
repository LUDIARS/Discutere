import { stringifyGameKG } from "./md-format.js";
import type { CrawlerRunner, RunOptions, RunResult } from "./runner.js";
import type { AestheticEntry, GameKG, MechanicEntry, SourceRef } from "./types.js";
import { DEFAULT_WORKSPACE } from "./types.js";
import type { LLMClient } from "../persona-engine/llm/client.js";

export interface AiTrainingRunnerOptions {
  llm: LLMClient;
  workspaceId?: string;
  model?: string;
  maxTokens?: number;
  timeoutMs?: number;
}

interface Critique {
  risks: string[];
  corrections: string[];
  missing: string[];
}

export function createAiTrainingRunner(
  opts: AiTrainingRunnerOptions
): CrawlerRunner {
  return new AiTrainingRunner(opts);
}

class AiTrainingRunner implements CrawlerRunner {
  constructor(private readonly opts: AiTrainingRunnerOptions) {}

  async run(gameName: string, options: RunOptions = {}): Promise<RunResult> {
    const draft = await invokeJson<GameKG>(this.opts.llm, {
      system: researcherSystem(),
      prompt: researcherPrompt(gameName, this.opts.workspaceId ?? DEFAULT_WORKSPACE, options),
      model: this.opts.model,
      maxTokens: this.opts.maxTokens ?? 2400,
      timeoutMs: this.opts.timeoutMs,
    });

    const critique = await invokeJson<Critique>(this.opts.llm, {
      system: criticSystem(),
      prompt: criticPrompt(gameName, draft),
      model: this.opts.model,
      maxTokens: Math.min(this.opts.maxTokens ?? 1600, 2000),
      timeoutMs: this.opts.timeoutMs,
    });

    const kg = await invokeJson<GameKG>(this.opts.llm, {
      system: synthesizerSystem(),
      prompt: synthesizerPrompt(gameName, draft, critique, this.opts.workspaceId ?? DEFAULT_WORKSPACE),
      model: this.opts.model,
      maxTokens: this.opts.maxTokens ?? 2400,
      timeoutMs: this.opts.timeoutMs,
    });

    const normalized = normalizeGameKG(kg, gameName, this.opts.workspaceId ?? DEFAULT_WORKSPACE);
    return {
      markdown: stringifyGameKG(normalized),
      kg: normalized,
      meta: {
        gameName,
        ranAt: Date.now(),
        crawlerVersion: "ai-training-v1",
        sourcesUsed: normalized.sources.map((source) => source.url),
      },
    };
  }
}

function researcherSystem(): string {
  return [
    "You are Discutere Researcher AI.",
    "Generate compact, source-aware game design learning data.",
    "Return only JSON matching the requested schema.",
    "Do not include copyrighted excerpts. Summarize in your own words.",
  ].join(" ");
}

function researcherPrompt(
  gameName: string,
  workspaceId: string,
  options: RunOptions
): string {
  return JSON.stringify({
    task: "Create an initial GameKG draft for Discutere training.",
    game_name: gameName,
    workspace_id: workspaceId,
    max_sources: options.maxSources ?? 5,
    excerpt_policy: options.excerptPolicy ?? "summary-only",
    schema: gameKgSchema(),
    guidance: [
      "Use stable public facts and common game-design knowledge.",
      "Prefer official pages, store pages, wikis, or documentation as source URLs when known.",
      "Mechanics should include intended player affect when reasonably inferable.",
      "Aesthetics should describe observed or likely player experience.",
    ],
  });
}

function criticSystem(): string {
  return [
    "You are Discutere Critic AI.",
    "Review another AI's GameKG draft for weak evidence, vague mechanics, and missing aesthetics.",
    "Return only JSON.",
  ].join(" ");
}

function criticPrompt(gameName: string, draft: GameKG): string {
  return JSON.stringify({
    task: "Critique this draft before it becomes training data.",
    game_name: gameName,
    draft,
    schema: {
      risks: ["string"],
      corrections: ["string"],
      missing: ["string"],
    },
  });
}

function synthesizerSystem(): string {
  return [
    "You are Discutere Synthesizer AI.",
    "Merge researcher and critic outputs into clean Discutere GameKG JSON.",
    "Return only JSON matching the GameKG schema.",
  ].join(" ");
}

function synthesizerPrompt(
  gameName: string,
  draft: GameKG,
  critique: Critique,
  workspaceId: string
): string {
  return JSON.stringify({
    task: "Produce the final GameKG training record.",
    game_name: gameName,
    workspace_id: workspaceId,
    draft,
    critique,
    schema: gameKgSchema(),
    rules: [
      "Keep only claims that are plausible and useful for game-design discussion.",
      "Remove empty fields.",
      "Use summary-only source policy unless a stricter policy is required.",
      "Add a short markdown body explaining how the AI debate changed the draft.",
    ],
  });
}

function gameKgSchema(): Record<string, unknown> {
  return {
    id: "kebab-case ASCII slug",
    title: "string",
    genre: "string optional",
    workspace_id: "string",
    sources: [
      {
        url: "string",
        title: "string optional",
        fetched_at: "ISO date optional",
        attribution: "string optional",
        excerpt_policy: "summary-only | fair-use-quote | forbidden",
      },
    ],
    mechanics: [
      {
        name: "string",
        description: "string optional",
        intends: "string optional",
        intended_affect: "string optional",
      },
    ],
    aesthetics: [
      {
        name: "string",
        description: "string optional",
      },
    ],
    body: "markdown string optional",
  };
}

async function invokeJson<T>(
  llm: LLMClient,
  args: {
    system: string;
    prompt: string;
    model?: string;
    maxTokens: number;
    timeoutMs?: number;
  }
): Promise<T> {
  const result = await llm.invoke(args);
  if (!result.ok) {
    throw new Error(result.error);
  }
  return parseJsonObject<T>(result.text);
}

export function parseJsonObject<T>(text: string): T {
  const trimmed = text.trim();
  const fenced = trimmed.match(/^```(?:json)?\s*([\s\S]*?)\s*```$/i);
  const candidate = fenced?.[1] ?? extractFirstJsonObject(trimmed);
  try {
    return JSON.parse(candidate) as T;
  } catch (err) {
    throw new Error(`LLM returned invalid JSON: ${(err as Error).message}`);
  }
}

function extractFirstJsonObject(text: string): string {
  const start = text.indexOf("{");
  const end = text.lastIndexOf("}");
  if (start === -1 || end === -1 || end <= start) {
    return text;
  }
  return text.slice(start, end + 1);
}

function normalizeGameKG(raw: GameKG, gameName: string, workspaceId: string): GameKG {
  const title = cleanString(raw.title) ?? gameName;
  const id = toSlug(cleanString(raw.id) ?? title);
  const sources = normalizeSources(raw.sources);
  const mechanics = normalizeMechanics(raw.mechanics);
  const aesthetics = normalizeAesthetics(raw.aesthetics);

  return {
    id,
    title,
    genre: cleanString(raw.genre),
    workspace_id: cleanString(raw.workspace_id) ?? workspaceId,
    sources,
    mechanics,
    aesthetics,
    body: cleanString(raw.body),
  };
}

function normalizeSources(raw: unknown): SourceRef[] {
  if (!Array.isArray(raw)) return [];
  return raw
    .map((entry): SourceRef | undefined => {
      if (!entry || typeof entry !== "object") return undefined;
      const source = entry as SourceRef;
      const url = cleanString(source.url);
      if (!url) return undefined;
      return compact({
        url,
        title: cleanString(source.title),
        fetched_at: cleanString(source.fetched_at),
        attribution: cleanString(source.attribution),
        excerpt_policy: normalizeExcerptPolicy(source.excerpt_policy),
      }) as SourceRef;
    })
    .filter((entry): entry is SourceRef => entry !== undefined);
}

function normalizeMechanics(raw: unknown): MechanicEntry[] {
  if (!Array.isArray(raw)) return [];
  return raw
    .map((entry): MechanicEntry | undefined => {
      if (!entry || typeof entry !== "object") return undefined;
      const mechanic = entry as MechanicEntry;
      const name = cleanString(mechanic.name);
      if (!name) return undefined;
      return compact({
        name,
        description: cleanString(mechanic.description),
        intends: cleanString(mechanic.intends),
        intended_affect: cleanString(mechanic.intended_affect),
      }) as MechanicEntry;
    })
    .filter((entry): entry is MechanicEntry => entry !== undefined);
}

function normalizeAesthetics(raw: unknown): AestheticEntry[] {
  if (!Array.isArray(raw)) return [];
  return raw
    .map((entry): AestheticEntry | undefined => {
      if (!entry || typeof entry !== "object") return undefined;
      const aesthetic = entry as AestheticEntry;
      const name = cleanString(aesthetic.name);
      if (!name) return undefined;
      return compact({
        name,
        description: cleanString(aesthetic.description),
      }) as AestheticEntry;
    })
    .filter((entry): entry is AestheticEntry => entry !== undefined);
}

function normalizeExcerptPolicy(value: unknown): SourceRef["excerpt_policy"] {
  if (value === "fair-use-quote" || value === "forbidden") return value;
  return "summary-only";
}

function cleanString(value: unknown): string | undefined {
  if (typeof value !== "string") return undefined;
  const trimmed = value.trim();
  return trimmed.length > 0 ? trimmed : undefined;
}

function toSlug(value: string): string {
  const slug = value
    .normalize("NFKD")
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .replace(/-{2,}/g, "-");
  return slug || "ai-generated-game";
}

function compact<T extends object>(obj: T): Partial<T> {
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(obj)) {
    if (value !== undefined) {
      out[key] = value;
    }
  }
  return out as Partial<T>;
}
