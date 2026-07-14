import { createHash } from "node:crypto";

import { DIM } from "./sentiment-vector.js";
import {
  findPoolPersonaByUserId,
  upsertPoolPersonaByUserId,
  type PoolPersona,
} from "./persona-pool.js";

const VOLUPTAS_USER_ID_PATTERN = /^ext:voluptas:[0-9a-f]{16}$/;
const SUPPORTED_VECTOR_SPEC_VERSION = 1;
const MAX_TRAITS = 32;
const MAX_TRAIT_LENGTH = 80;

export interface VoluptasPersonaPayload {
  user_id: string;
  affect_vector: number[];
  vector_spec_version: number;
  traits?: string[];
}

export interface PersonaImportSummary {
  imported: number;
  inserted: number;
  updated: number;
}

function requiredRecord(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new TypeError("persona import item must be an object");
  }
  return value as Record<string, unknown>;
}

function parseUserId(value: unknown): string {
  if (typeof value !== "string" || !VOLUPTAS_USER_ID_PATTERN.test(value)) {
    throw new TypeError("user_id must be a Voluptas HMAC pseudonym (ext:voluptas:<16 hex>)");
  }
  return value;
}

function parseVector(value: unknown): number[] {
  if (!Array.isArray(value) || value.length !== DIM) {
    throw new TypeError(`affect_vector must contain exactly ${DIM} values`);
  }
  return value.map((item) => {
    if (typeof item !== "number" || !Number.isFinite(item)) {
      throw new TypeError("affect_vector values must be finite numbers");
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
  return [...new Set(traits.filter(Boolean))].slice(0, MAX_TRAITS).map((trait) => trait.slice(0, MAX_TRAIT_LENGTH));
}

export function parseVoluptasPersonaPayload(value: unknown): VoluptasPersonaPayload {
  const input = requiredRecord(value);
  const vectorSpecVersion = input.vector_spec_version;
  if (vectorSpecVersion !== SUPPORTED_VECTOR_SPEC_VERSION) {
    throw new TypeError(`unsupported vector_spec_version: ${String(vectorSpecVersion)}`);
  }
  return {
    user_id: parseUserId(input.user_id),
    affect_vector: parseVector(input.affect_vector),
    vector_spec_version: vectorSpecVersion,
    traits: parseTraits(input.traits),
  };
}

export function parseVoluptasPersonaBatch(value: unknown): VoluptasPersonaPayload[] {
  const candidate = value && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>).personas
    : value;
  if (!Array.isArray(candidate)) throw new TypeError("persona import payload must be an array or { personas: [] }");
  return candidate.map(parseVoluptasPersonaPayload);
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
    id: anonymousPersonaId(payload.user_id),
    name: anonymousPersonaName(payload.user_id),
    role: "opinion",
    speechStyle: "",
    traits: payload.traits ?? [],
    affectVector: payload.affect_vector,
    origin: "imported",
    parentIds: [],
    learningSource: "voluptas",
    label: "Voluptas 匿名プロフィール",
    sourceSpeakerId: `ext:feedback:${payload.user_id}`,
    userId: payload.user_id,
  };
}

export function importVoluptasPersonas(value: unknown): PersonaImportSummary {
  const payloads = parseVoluptasPersonaBatch(value);
  let inserted = 0;
  let updated = 0;
  for (const payload of payloads) {
    if (findPoolPersonaByUserId(payload.user_id)) updated += 1;
    else inserted += 1;
    upsertPoolPersonaByUserId(toPoolPersona(payload));
  }
  return { imported: payloads.length, inserted, updated };
}
