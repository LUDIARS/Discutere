import { createHash } from "node:crypto";

import { DIM } from "./sentiment-vector.js";
import {
  findPoolPersonaByUserId,
  upsertPoolPersonaByUserId,
  type PersonaAversion,
  type PersonaMechanicReaction,
  type PoolPersona,
} from "./persona-pool.js";

// @spec インポート (Vo → Di)
const VOLUPTAS_USER_ID_PATTERN = /^ext:voluptas:[0-9a-f]{16}$/;
const SUPPORTED_VECTOR_SPEC_VERSION = 1;
const SUPPORTED_EXPORT_SPEC_VERSION = 2;
const MAX_TRAITS = 32;
const MAX_TRAIT_LENGTH = 80;
const PREFERENCE_AXIS_PATTERN = /^(mtg|style)\.[a-z][a-z0-9_]*$/;

export interface VoluptasPersonaPayload {
  pseudoId: string;
  affectVector: number[];
  vectorSpecVersion: number;
  traits: string[];
  preferenceAxes: Record<string, number>;
  attributes: { ageBand?: string; spending?: string };
  aversions: PersonaAversion[];
  mechanicReactions: PersonaMechanicReaction[];
  exportSpecVersion?: number;
}

export interface PersonaImportSummary {
  imported: number;
  inserted: number;
  updated: number;
  skipped: number;
  skipReasons: Record<string, number>;
  dryRun: boolean;
}

function requiredRecord(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new TypeError("persona import item must be an object");
  }
  return value as Record<string, unknown>;
}

function parsePseudoId(value: unknown): string {
  if (typeof value !== "string" || !VOLUPTAS_USER_ID_PATTERN.test(value)) {
    throw new TypeError("pseudoId must be a Voluptas HMAC pseudonym (ext:voluptas:<16 hex>)");
  }
  return value;
}

function parseVector(value: unknown): number[] {
  if (!Array.isArray(value) || value.length !== DIM) {
    throw new TypeError(`affectVector must contain exactly ${DIM} values`);
  }
  return value.map((item) => {
    if (typeof item !== "number" || !Number.isFinite(item) || item < 0 || item > 1) {
      throw new TypeError("affectVector values must be finite numbers in 0..1");
    }
    return item;
  });
}

function parseTraits(value: unknown): string[] {
  if (value === undefined) return [];
  if (!Array.isArray(value)) throw new TypeError("traits must be an array of strings");
  const traits = value.map((item) => {
    if (typeof item !== "string") throw new TypeError("traits must be an array of strings");
    return item.trim();
  });
  // 切り詰めてから重複除去する (先に除去すると切り詰めで重複が復活する)。
  return [...new Set(
    traits.filter(Boolean).map((trait) => trait.slice(0, MAX_TRAIT_LENGTH))
  )].slice(0, MAX_TRAITS);
}

function parsePreferenceAxes(value: unknown): Record<string, number> {
  if (value === undefined) return {};
  const input = requiredRecord(value);
  const entries: Array<[string, number]> = Object.entries(input).map(([axis, score]) => {
    if (
      !PREFERENCE_AXIS_PATTERN.test(axis)
      || typeof score !== "number"
      || !Number.isFinite(score)
      || score < 0
      || score > 1
    ) {
      throw new TypeError("preferenceAxes must contain canonical axis scores in 0..1");
    }
    return [axis, score];
  });
  return Object.fromEntries(entries.sort(([left], [right]) => left.localeCompare(right)));
}

function parseAttributes(value: unknown): VoluptasPersonaPayload["attributes"] {
  if (value === undefined) return {};
  const input = requiredRecord(value);
  return Object.fromEntries(["ageBand", "spending"].flatMap((key) => {
    const item = input[key];
    if (item === undefined || item === null || item === "") return [];
    if (typeof item !== "string" || item.length > 80) {
      throw new TypeError(`attributes.${key} must be a string up to 80 characters`);
    }
    return [[key, item.trim()]];
  }));
}

function parseAversions(value: unknown): PersonaAversion[] {
  if (value === undefined) return [];
  if (!Array.isArray(value)) throw new TypeError("aversions must be an array");
  return value.map((item) => {
    const input = requiredRecord(item);
    if (
      typeof input.target !== "string"
      || !input.target.trim()
      || input.target.length > 160
      || typeof input.strength !== "number"
      || !Number.isFinite(input.strength)
      || input.strength < 0
      || input.strength > 1
    ) {
      throw new TypeError("aversions need a target and strength in 0..1");
    }
    return { target: input.target.trim(), strength: input.strength };
  });
}

function parseMechanicReactions(value: unknown): PersonaMechanicReaction[] {
  if (value === undefined) return [];
  if (!Array.isArray(value)) throw new TypeError("mechanicReactions must be an array");
  return value.map((item) => {
    const input = requiredRecord(item);
    if (
      typeof input.mechanicId !== "string"
      || !/^[a-z0-9][a-z0-9/_-]{0,119}$/.test(input.mechanicId)
      || typeof input.sentiment !== "number"
      || !Number.isFinite(input.sentiment)
      || input.sentiment < -2
      || input.sentiment > 2
    ) {
      throw new TypeError("mechanicReactions need a mechanicId and sentiment in -2..2");
    }
    return { mechanicId: input.mechanicId, sentiment: input.sentiment };
  });
}

function parseVersionedPersonaCore(
  input: Record<string, unknown>
): Pick<
  VoluptasPersonaPayload,
  "pseudoId" | "affectVector" | "vectorSpecVersion" | "exportSpecVersion"
> {
  const exportSpecVersion = input.exportSpecVersion;
  if (exportSpecVersion !== undefined && exportSpecVersion !== SUPPORTED_EXPORT_SPEC_VERSION) {
    throw new TypeError(`unsupported exportSpecVersion: ${String(exportSpecVersion)}`);
  }
  const vectorSpecVersion = input.vectorSpecVersion ?? input.vector_spec_version;
  if (vectorSpecVersion !== SUPPORTED_VECTOR_SPEC_VERSION) {
    throw new TypeError(`unsupported vectorSpecVersion: ${String(vectorSpecVersion)}`);
  }
  return {
    pseudoId: parsePseudoId(input.pseudoId ?? input.user_id),
    affectVector: parseVector(input.affectVector ?? input.affect_vector),
    vectorSpecVersion,
    ...(exportSpecVersion === undefined ? {} : { exportSpecVersion }),
  };
}

function parsePersonaCompartments(
  input: Record<string, unknown>
): Pick<
  VoluptasPersonaPayload,
  "traits" | "preferenceAxes" | "attributes" | "aversions" | "mechanicReactions"
> {
  return {
    traits: parseTraits(input.traits),
    preferenceAxes: parsePreferenceAxes(input.preferenceAxes),
    attributes: parseAttributes(input.attributes),
    aversions: parseAversions(input.aversions),
    mechanicReactions: parseMechanicReactions(input.mechanicReactions),
  };
}

export function parseVoluptasPersonaPayload(value: unknown): VoluptasPersonaPayload {
  const input = requiredRecord(value);
  return {
    ...parseVersionedPersonaCore(input),
    ...parsePersonaCompartments(input),
  };
}

export function parseVoluptasPersonaBatch(value: unknown): unknown[] {
  const candidate = value && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>).personas
    : value;
  if (!Array.isArray(candidate)) {
    throw new TypeError("persona import payload must be an array or { personas: [] }");
  }
  return candidate;
}

function anonymousPersonaId(userId: string): string {
  return `persona:voluptas:${createHash("sha256").update(userId, "utf8").digest("hex").slice(0, 16)}`;
}

function anonymousPersonaName(userId: string): string {
  const suffix = createHash("sha256").update(userId, "utf8").digest("hex").slice(0, 8);
  return `プレイヤー#${suffix}`;
}

function toPoolPersona(payload: VoluptasPersonaPayload): PoolPersona {
  return {
    id: anonymousPersonaId(payload.pseudoId),
    name: anonymousPersonaName(payload.pseudoId),
    role: "opinion",
    speechStyle: "",
    traits: payload.traits,
    affectVector: payload.affectVector,
    origin: "imported",
    parentIds: [],
    learningSource: "voluptas",
    label: "Voluptas 匿名プロフィール",
    sourceSpeakerId: `ext:feedback:${payload.pseudoId}`,
    userId: payload.pseudoId,
    preferenceAxes: payload.preferenceAxes,
    attributes: payload.attributes,
    aversions: payload.aversions,
    mechanicReactions: payload.mechanicReactions,
    exportSpecVersion: payload.exportSpecVersion,
  };
}

function reasonCode(error: unknown): string {
  const message = error instanceof Error ? error.message : String(error);
  if (/exportSpecVersion/.test(message)) return "export-spec";
  if (/vectorSpecVersion/.test(message)) return "vector-spec";
  if (/affectVector/.test(message)) return "affect-vector";
  if (/pseudoId/.test(message)) return "pseudo-id";
  return "invalid-payload";
}

function countSkip(summary: PersonaImportSummary, reason: string): void {
  summary.skipped += 1;
  summary.skipReasons[reason] = (summary.skipReasons[reason] ?? 0) + 1;
}

export function importVoluptasPersonas(
  value: unknown,
  options: { dryRun?: boolean; invalidJsonLines?: number } = {}
): PersonaImportSummary {
  const candidates = parseVoluptasPersonaBatch(value);
  // NDJSON の壊れた行は取込前に落ちているので、呼び出し側から件数だけ受けて skip に合算する。
  const invalidJsonLines = options.invalidJsonLines ?? 0;
  const summary: PersonaImportSummary = {
    imported: 0,
    inserted: 0,
    updated: 0,
    skipped: invalidJsonLines,
    skipReasons: invalidJsonLines > 0 ? { "invalid-json": invalidJsonLines } : {},
    dryRun: options.dryRun === true,
  };
  for (const candidate of candidates) {
    let payload: VoluptasPersonaPayload;
    try {
      payload = parseVoluptasPersonaPayload(candidate);
    } catch (error) {
      countSkip(summary, reasonCode(error));
      continue;
    }
    // アーカイブ済みの同一人物も「更新」扱い (upsert が復帰させる)。
    const exists = findPoolPersonaByUserId(payload.pseudoId, { includeArchived: true });
    try {
      if (!summary.dryRun) upsertPoolPersonaByUserId(toPoolPersona(payload));
    } catch {
      // 1 行の書き込み失敗で残りの取込を落とさない (spec persona-bridge §1.3)。
      countSkip(summary, "write-failed");
      continue;
    }
    if (exists) summary.updated += 1;
    else summary.inserted += 1;
    summary.imported += 1;
  }
  return summary;
}
