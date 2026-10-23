/**
 * 代理議論の人物パック契約 v1 (spec/interface/proxy-persona-pack.md)。
 *
 * 形成側 (Histrio、当面は scripts/proxy-form-packs.ts) が 1 話者 × 1 ジャンルの人物像を作り、
 * Di はこの契約を検証してから取り込む。検証は入口で全部行い、通らない行は理由付きで拒否する:
 *   - speakerKey は `ext:<source>:<hmac16>` の仮名だけ (生 ID・表示名を通さない)
 *   - 出典 URL・表示名・生の外部 ID に当たるフィールドを含む行は拒否 (U3: URL は渡さない)
 *   - 立場カードの evidenceIds は同じパックの evidence を指す (根拠の無い主張を入れない)
 *   - 20 次元ベクトルは sentiment-core v1 と同じ次元・有限値
 *
 * 判断 (キャストに出すか・束の作り方) は持たない。型と検証だけ (SRP)。
 */

import { DIM, VECTOR_SPEC_VERSION } from "../sentiment-vector.js";

export const PROXY_CONTRACT_VERSION = 1;

/** `ext:<source>:<hmac16>`。source は英小文字。末尾は形成側の HMAC (16 hex)。 */
export const SPEAKER_KEY_PATTERN = /^ext:[a-z][a-z0-9_-]*:[0-9a-f]{16}$/;
const GENRE_PATTERN = /^[a-z0-9][a-z0-9_-]{0,63}$/;
const EVIDENCE_ID_PATTERN = /^E\d{1,3}$/;
const STANCE_CARD_ID_PATTERN = /^S\d{1,3}$/;

export const MAX_EVIDENCE = 12;
export const MAX_EVIDENCE_CHARS = 400;
export const MAX_STANCE_CARDS = 8;
const MAX_CLAIM_CHARS = 200;
const MAX_STYLE_NOTES = 4;
const MAX_STYLE_NOTE_CHARS = 80;
const MAX_HELD_OUT = 64;

/** 個人を特定しうる / U3 で渡さないと決めたフィールド名。どの階層に現れても拒否する。 */
const FORBIDDEN_KEYS = new Set([
  "url",
  "sourceUrl",
  "source_url",
  "authorId",
  "author_id",
  "authorName",
  "author_name",
  "displayName",
  "display_name",
  "steamId",
  "steam_id",
  "userId",
  "user_id",
]);
/** 文字列値に紛れた URL も拒否する (本文中のリンクで出典へ辿れないように)。 */
const URL_IN_TEXT = /https?:\/\//i;

export type Polarity = "pos" | "neg";
export type Confidence = "high" | "medium" | "low";
export type StanceExtractor = "rule" | "llm";

export interface PackSufficiency {
  utterances: number;
  positive: number;
  negative: number;
  games: number;
  aspects: number;
  confidence: Confidence;
  /** 不適格・低信頼の理由 (形成側の申告)。 */
  reasons: string[];
}

export interface ConditionalGap {
  current: string;
  desired: string;
  condition: string | null;
  evidenceIds: string[];
}

export interface PackDesignGap {
  /** 20D の数値差 (正の発言 − 負の発言)。作れなければ null。prompt には出さない。 */
  vector: number[] | null;
  method: "pos-minus-neg";
  /** 望む側に強い次元 (sentiment-core の次元名)。 */
  wants: string[];
  /** 不満側に強い次元。 */
  complaints: string[];
  /** vector が null のときの理由。 */
  reason: string | null;
}

export interface StanceCard {
  id: string;
  claim: string;
  polarity: Polarity;
  evidenceIds: string[];
  extractor: StanceExtractor;
}

export interface PackEvidence {
  id: string;
  /** 形成側の原文 revision (内容 hash 等)。引用の検証と失効の追跡に使う。 */
  revision: string;
  text: string;
  sourceKind: string;
  gameTitle: string;
  polarity: Polarity;
  postedAt: number | null;
}

export interface HeldOutLabel {
  game: string;
  recommended: boolean;
}

export interface PersonaPack {
  contractVersion: number;
  personaKind: "observedSpeaker";
  speakerKey: string;
  genre: string;
  genreSpecVersion: number;
  profileVersion: string;
  formation: { method: StanceExtractor; ruleVersion: string };
  window: { observedMonths: number | null; generatedAt: number; expiresAt: number };
  sufficiency: PackSufficiency;
  affect: { vector: number[]; vectorSpecVersion: number };
  designGap: PackDesignGap;
  conditionalGaps: ConditionalGap[];
  stanceCards: StanceCard[];
  evidence: PackEvidence[];
  styleNotes: string[];
  likedGames: string[];
  heldOut: HeldOutLabel[];
  revoked: boolean;
}

export type PackValidation = { ok: true; pack: PersonaPack } | { ok: false; errors: string[] };

// ── 低レベルの取り出し ─────────────────────────────────────────────────────────

type Obj = Record<string, unknown>;

function isObj(v: unknown): v is Obj {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}

function findForbidden(v: unknown, at: string, out: string[]): void {
  if (typeof v === "string") {
    if (URL_IN_TEXT.test(v)) out.push(`${at}: URL を含む値は受け取らない`);
    return;
  }
  if (Array.isArray(v)) {
    v.forEach((item, i) => findForbidden(item, `${at}[${i}]`, out));
    return;
  }
  if (!isObj(v)) return;
  for (const [key, value] of Object.entries(v)) {
    if (FORBIDDEN_KEYS.has(key)) out.push(`${at}.${key}: 個人を特定しうるフィールドは受け取らない`);
    findForbidden(value, `${at}.${key}`, out);
  }
}

function str(v: unknown, at: string, errors: string[], max = 2000): string {
  if (typeof v !== "string" || !v.trim()) {
    errors.push(`${at}: 空でない文字列が必要`);
    return "";
  }
  const s = v.trim();
  if (s.length > max) errors.push(`${at}: ${max} 文字以内`);
  return s;
}

function num(v: unknown, at: string, errors: string[]): number {
  if (typeof v !== "number" || !Number.isFinite(v)) {
    errors.push(`${at}: 有限の数値が必要`);
    return 0;
  }
  return v;
}

function nonNegInt(v: unknown, at: string, errors: string[]): number {
  const n = num(v, at, errors);
  if (!Number.isInteger(n) || n < 0) errors.push(`${at}: 0 以上の整数`);
  return n;
}

function strList(v: unknown, at: string, errors: string[], maxItems: number, maxChars: number): string[] {
  if (v === undefined) return [];
  if (!Array.isArray(v)) {
    errors.push(`${at}: 配列が必要`);
    return [];
  }
  if (v.length > maxItems) errors.push(`${at}: ${maxItems} 件以内`);
  return v.slice(0, maxItems).map((item, i) => str(item, `${at}[${i}]`, errors, maxChars));
}

function vector20(v: unknown, at: string, errors: string[]): number[] {
  if (!Array.isArray(v) || v.length !== DIM || v.some((x) => typeof x !== "number" || !Number.isFinite(x))) {
    errors.push(`${at}: ${DIM} 次元の有限値ベクトルが必要`);
    return [];
  }
  return v as number[];
}

function polarity(v: unknown, at: string, errors: string[]): Polarity {
  if (v === "pos" || v === "neg") return v;
  errors.push(`${at}: pos | neg`);
  return "pos";
}

function evidenceIds(v: unknown, at: string, errors: string[], known: ReadonlySet<string>): string[] {
  const ids = strList(v, at, errors, MAX_EVIDENCE, 8);
  for (const id of ids) {
    if (!known.has(id)) errors.push(`${at}: ${id} はこのパックの evidence に無い`);
  }
  return ids;
}

// ── 部分ごとの検証 ───────────────────────────────────────────────────────────

function parseEvidence(v: unknown, errors: string[]): PackEvidence[] {
  if (!Array.isArray(v) || v.length === 0) {
    errors.push("evidence: 1 件以上の配列が必要");
    return [];
  }
  if (v.length > MAX_EVIDENCE) errors.push(`evidence: ${MAX_EVIDENCE} 件以内`);
  const seen = new Set<string>();
  return v.slice(0, MAX_EVIDENCE).map((raw, i) => {
    const at = `evidence[${i}]`;
    const o = isObj(raw) ? raw : {};
    const id = str(o.id, `${at}.id`, errors, 8);
    if (id && !EVIDENCE_ID_PATTERN.test(id)) errors.push(`${at}.id: E<数字>`);
    if (seen.has(id)) errors.push(`${at}.id: ${id} が重複`);
    seen.add(id);
    const postedAt = o.postedAt === null || o.postedAt === undefined ? null : num(o.postedAt, `${at}.postedAt`, errors);
    return {
      id,
      revision: str(o.revision, `${at}.revision`, errors, 128),
      text: str(o.text, `${at}.text`, errors, MAX_EVIDENCE_CHARS),
      sourceKind: str(o.sourceKind, `${at}.sourceKind`, errors, 32),
      gameTitle: str(o.gameTitle, `${at}.gameTitle`, errors, 200),
      polarity: polarity(o.polarity, `${at}.polarity`, errors),
      postedAt,
    };
  });
}

function parseStanceCards(v: unknown, errors: string[], known: ReadonlySet<string>): StanceCard[] {
  if (v === undefined) return [];
  if (!Array.isArray(v)) {
    errors.push("stanceCards: 配列が必要");
    return [];
  }
  if (v.length > MAX_STANCE_CARDS) errors.push(`stanceCards: ${MAX_STANCE_CARDS} 件以内`);
  return v.slice(0, MAX_STANCE_CARDS).map((raw, i) => {
    const at = `stanceCards[${i}]`;
    const o = isObj(raw) ? raw : {};
    const id = str(o.id, `${at}.id`, errors, 8);
    if (id && !STANCE_CARD_ID_PATTERN.test(id)) errors.push(`${at}.id: S<数字>`);
    const ids = evidenceIds(o.evidenceIds, `${at}.evidenceIds`, errors, known);
    if (ids.length === 0) errors.push(`${at}.evidenceIds: 根拠の無い立場カードは受け取らない`);
    const extractor = o.extractor === "llm" ? "llm" : o.extractor === "rule" ? "rule" : null;
    if (!extractor) errors.push(`${at}.extractor: rule | llm`);
    return {
      id,
      claim: str(o.claim, `${at}.claim`, errors, MAX_CLAIM_CHARS),
      polarity: polarity(o.polarity, `${at}.polarity`, errors),
      evidenceIds: ids,
      extractor: extractor ?? "rule",
    };
  });
}

function parseConditionalGaps(v: unknown, errors: string[], known: ReadonlySet<string>): ConditionalGap[] {
  if (v === undefined) return [];
  if (!Array.isArray(v)) {
    errors.push("conditionalGaps: 配列が必要");
    return [];
  }
  return v.slice(0, MAX_STANCE_CARDS).map((raw, i) => {
    const at = `conditionalGaps[${i}]`;
    const o = isObj(raw) ? raw : {};
    const ids = evidenceIds(o.evidenceIds, `${at}.evidenceIds`, errors, known);
    if (ids.length === 0) errors.push(`${at}.evidenceIds: 根拠が必要`);
    return {
      current: str(o.current, `${at}.current`, errors, MAX_CLAIM_CHARS),
      desired: str(o.desired, `${at}.desired`, errors, MAX_CLAIM_CHARS),
      condition: o.condition === null || o.condition === undefined ? null : str(o.condition, `${at}.condition`, errors, MAX_CLAIM_CHARS),
      evidenceIds: ids,
    };
  });
}

function parseSufficiency(v: unknown, errors: string[]): PackSufficiency {
  const o = isObj(v) ? v : {};
  if (!isObj(v)) errors.push("sufficiency: オブジェクトが必要");
  const confidence = o.confidence === "high" || o.confidence === "medium" || o.confidence === "low" ? o.confidence : null;
  if (!confidence) errors.push("sufficiency.confidence: high | medium | low");
  return {
    utterances: nonNegInt(o.utterances, "sufficiency.utterances", errors),
    positive: nonNegInt(o.positive, "sufficiency.positive", errors),
    negative: nonNegInt(o.negative, "sufficiency.negative", errors),
    games: nonNegInt(o.games, "sufficiency.games", errors),
    aspects: nonNegInt(o.aspects, "sufficiency.aspects", errors),
    confidence: confidence ?? "low",
    reasons: strList(o.reasons, "sufficiency.reasons", errors, 8, 200),
  };
}

function parseDesignGap(v: unknown, errors: string[]): PackDesignGap {
  const o = isObj(v) ? v : {};
  if (!isObj(v)) errors.push("designGap: オブジェクトが必要");
  if (o.method !== "pos-minus-neg") errors.push("designGap.method: pos-minus-neg");
  const vector = o.vector === null || o.vector === undefined ? null : vector20(o.vector, "designGap.vector", errors);
  const reason = o.reason === null || o.reason === undefined ? null : str(o.reason, "designGap.reason", errors, 200);
  if (vector === null && !reason) errors.push("designGap.reason: vector が null なら理由が必要");
  return {
    vector,
    method: "pos-minus-neg",
    wants: strList(o.wants, "designGap.wants", errors, 6, 40),
    complaints: strList(o.complaints, "designGap.complaints", errors, 6, 40),
    reason,
  };
}

function parseHeldOut(v: unknown, errors: string[]): HeldOutLabel[] {
  if (v === undefined) return [];
  if (!Array.isArray(v)) {
    errors.push("heldOut: 配列が必要");
    return [];
  }
  return v.slice(0, MAX_HELD_OUT).map((raw, i) => {
    const o = isObj(raw) ? raw : {};
    if (typeof o.recommended !== "boolean") errors.push(`heldOut[${i}].recommended: boolean が必要`);
    return { game: str(o.game, `heldOut[${i}].game`, errors, 200), recommended: o.recommended === true };
  });
}

/** 人物パック 1 件を検証する。通れば正規化済みの PersonaPack を返す。 */
export function parsePersonaPack(raw: unknown): PackValidation {
  const errors: string[] = [];
  if (!isObj(raw)) return { ok: false, errors: ["パックはオブジェクトでなければならない"] };
  findForbidden(raw, "$", errors);

  if (raw.contractVersion !== PROXY_CONTRACT_VERSION) {
    errors.push(`contractVersion: ${PROXY_CONTRACT_VERSION} のみ対応`);
  }
  if (raw.personaKind !== "observedSpeaker") errors.push("personaKind: observedSpeaker のみ対応");
  const speakerKey = str(raw.speakerKey, "speakerKey", errors, 80);
  if (speakerKey && !SPEAKER_KEY_PATTERN.test(speakerKey)) {
    errors.push("speakerKey: ext:<source>:<hmac16> の仮名のみ受け取る");
  }
  const genre = str(raw.genre, "genre", errors, 64);
  if (genre && !GENRE_PATTERN.test(genre)) errors.push("genre: 辞書 id (英小文字・数字・-_)");

  const evidence = parseEvidence(raw.evidence, errors);
  const known = new Set(evidence.map((e) => e.id));

  const formation = isObj(raw.formation) ? raw.formation : {};
  const method = formation.method === "llm" ? "llm" : formation.method === "rule" ? "rule" : null;
  if (!method) errors.push("formation.method: rule | llm");
  const window = isObj(raw.window) ? raw.window : {};
  const generatedAt = num(window.generatedAt, "window.generatedAt", errors);
  const expiresAt = num(window.expiresAt, "window.expiresAt", errors);
  if (expiresAt <= generatedAt) errors.push("window.expiresAt: generatedAt より後");
  const affect = isObj(raw.affect) ? raw.affect : {};
  if (affect.vectorSpecVersion !== VECTOR_SPEC_VERSION) errors.push(`affect.vectorSpecVersion: ${VECTOR_SPEC_VERSION} のみ対応`);
  if (typeof raw.revoked !== "boolean") errors.push("revoked: boolean が必要");

  const pack: PersonaPack = {
    contractVersion: PROXY_CONTRACT_VERSION,
    personaKind: "observedSpeaker",
    speakerKey,
    genre,
    genreSpecVersion: nonNegInt(raw.genreSpecVersion, "genreSpecVersion", errors),
    profileVersion: str(raw.profileVersion, "profileVersion", errors, 128),
    formation: { method: method ?? "rule", ruleVersion: str(formation.ruleVersion, "formation.ruleVersion", errors, 64) },
    window: {
      observedMonths:
        window.observedMonths === null || window.observedMonths === undefined
          ? null
          : nonNegInt(window.observedMonths, "window.observedMonths", errors),
      generatedAt,
      expiresAt,
    },
    sufficiency: parseSufficiency(raw.sufficiency, errors),
    affect: { vector: vector20(affect.vector, "affect.vector", errors), vectorSpecVersion: VECTOR_SPEC_VERSION },
    designGap: parseDesignGap(raw.designGap, errors),
    conditionalGaps: parseConditionalGaps(raw.conditionalGaps, errors, known),
    stanceCards: parseStanceCards(raw.stanceCards, errors, known),
    evidence,
    styleNotes: strList(raw.styleNotes, "styleNotes", errors, MAX_STYLE_NOTES, MAX_STYLE_NOTE_CHARS),
    likedGames: strList(raw.likedGames, "likedGames", errors, 32, 200),
    heldOut: parseHeldOut(raw.heldOut, errors),
    revoked: raw.revoked === true,
  };
  return errors.length > 0 ? { ok: false, errors } : { ok: true, pack };
}
