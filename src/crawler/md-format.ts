/**
 * Crawler md frontmatter <-> GameKG round-trip.
 *
 * data/games/<slug>.md の構造:
 *   ---
 *   <YAML frontmatter — gray-matter で parse>
 *   ---
 *   <自由 markdown 本文 — DB には入れない>
 *
 * 必須キー: id, title, workspace_id
 * 配列キー: sources, mechanics, aesthetics は空配列に正規化される
 */

import matter from "gray-matter";

import type { AestheticEntry, GameKG, MechanicEntry, SourceRef } from "./types.js";
import { DEFAULT_WORKSPACE } from "./types.js";

const SLUG_RE = /^[a-z0-9][a-z0-9-]*$/;

export class GameKGParseError extends Error {
  constructor(message: string, public readonly path?: string) {
    super(path ? `${message} (file: ${path})` : message);
    this.name = "GameKGParseError";
  }
}

export function parseGameKG(markdown: string, sourcePath?: string): GameKG {
  const { data, content } = matter(markdown);

  if (typeof data.id !== "string" || !SLUG_RE.test(data.id)) {
    throw new GameKGParseError(
      `frontmatter.id is required and must be kebab-case ASCII (got ${JSON.stringify(data.id)})`,
      sourcePath
    );
  }
  if (typeof data.title !== "string" || data.title.trim() === "") {
    throw new GameKGParseError("frontmatter.title is required", sourcePath);
  }

  const workspaceId =
    typeof data.workspace_id === "string" && data.workspace_id.trim() !== ""
      ? data.workspace_id
      : DEFAULT_WORKSPACE;

  return {
    id: data.id,
    title: data.title,
    genre: typeof data.genre === "string" ? data.genre : undefined,
    workspace_id: workspaceId,
    sources: normalizeSources(data.sources),
    mechanics: normalizeMechanics(data.mechanics),
    aesthetics: normalizeAesthetics(data.aesthetics),
    body: content.trim() ? content : undefined,
  };
}

export function stringifyGameKG(kg: GameKG): string {
  const frontmatter: Record<string, unknown> = {
    id: kg.id,
    title: kg.title,
  };
  if (kg.genre !== undefined) frontmatter.genre = kg.genre;
  frontmatter.workspace_id = kg.workspace_id;
  if (kg.sources.length > 0) frontmatter.sources = kg.sources.map(compact);
  if (kg.mechanics.length > 0) frontmatter.mechanics = kg.mechanics.map(compact);
  if (kg.aesthetics.length > 0) frontmatter.aesthetics = kg.aesthetics.map(compact);

  return matter.stringify(kg.body ?? "", frontmatter);
}

/** undefined を含むキーを除外。 YAML dumper が undefined を扱えないため。 */
function compact<T extends object>(obj: T): Partial<T> {
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(obj)) {
    if (v !== undefined) out[k] = v;
  }
  return out as Partial<T>;
}

function normalizeSources(raw: unknown): SourceRef[] {
  if (!Array.isArray(raw)) return [];
  return raw.map((entry, idx) => {
    if (!entry || typeof entry !== "object") {
      throw new GameKGParseError(`sources[${idx}] is not an object`);
    }
    const e = entry as Record<string, unknown>;
    if (typeof e.url !== "string" || e.url.trim() === "") {
      throw new GameKGParseError(`sources[${idx}].url is required`);
    }
    const policy = e.excerpt_policy;
    if (
      policy !== undefined &&
      policy !== "summary-only" &&
      policy !== "fair-use-quote" &&
      policy !== "forbidden"
    ) {
      throw new GameKGParseError(
        `sources[${idx}].excerpt_policy must be one of summary-only / fair-use-quote / forbidden`
      );
    }
    return {
      url: e.url,
      title: typeof e.title === "string" ? e.title : undefined,
      fetched_at: typeof e.fetched_at === "string" ? e.fetched_at : undefined,
      attribution: typeof e.attribution === "string" ? e.attribution : undefined,
      excerpt_policy: policy as SourceRef["excerpt_policy"] | undefined,
    };
  });
}

function normalizeMechanics(raw: unknown): MechanicEntry[] {
  if (!Array.isArray(raw)) return [];
  return raw.map((entry, idx) => {
    if (!entry || typeof entry !== "object") {
      throw new GameKGParseError(`mechanics[${idx}] is not an object`);
    }
    const e = entry as Record<string, unknown>;
    if (typeof e.name !== "string" || e.name.trim() === "") {
      throw new GameKGParseError(`mechanics[${idx}].name is required`);
    }
    return {
      name: e.name,
      description: typeof e.description === "string" ? e.description : undefined,
      intends: typeof e.intends === "string" ? e.intends : undefined,
      intended_affect:
        typeof e.intended_affect === "string" ? e.intended_affect : undefined,
    };
  });
}

function normalizeAesthetics(raw: unknown): AestheticEntry[] {
  if (!Array.isArray(raw)) return [];
  return raw.map((entry, idx) => {
    if (!entry || typeof entry !== "object") {
      throw new GameKGParseError(`aesthetics[${idx}] is not an object`);
    }
    const e = entry as Record<string, unknown>;
    if (typeof e.name !== "string" || e.name.trim() === "") {
      throw new GameKGParseError(`aesthetics[${idx}].name is required`);
    }
    return {
      name: e.name,
      description: typeof e.description === "string" ? e.description : undefined,
    };
  });
}
