/**
 * 匿名ペルソナプール + 嗜好近傍選定 (憑依/壁打ち相手の共通基盤)。
 *
 * - flow_persona: Voluptas 取込または公開レビュー話者の採用で得た永続ペルソナ。
 * - selectByAffinity: ベクトル最近傍 (cosine) でペルソナを選ぶ = 「憑依」/壁打ち相手の選定。
 *
 * ベクトル空間は sentiment-vector.ts の固定 20 次元 ([[feedback]]: 新空間は作らない)。
 * テキストからのベクトル化は textToVector を流用する。
 */

// @spec 憑依 descriptor の強化
import { getFlowDb } from "./db/connection.js";
import { buildPersonaDescriptor } from "./persona-descriptor.js";
import { cosine, textToVector, DIM } from "./sentiment-vector.js";
import { roleDefaultStance, type FlowPersona, type FlowRole, type FlowStance } from "./personas.js";

export type PersonaOrigin = "seed" | "adopted" | "imported";

export interface PersonaAversion {
  target: string;
  strength: number;
}

export interface PersonaMechanicReaction {
  mechanicId: string;
  sentiment: number;
}

export interface PoolPersona {
  id: string;
  name: string;
  role: FlowRole;
  speechStyle: string;
  traits: string[];
  /** 固定 20 次元 affect ベクトル。 */
  affectVector: number[];
  origin: PersonaOrigin;
  /** 旧データとの互換用。新規の合成生成は行わない。 */
  parentIds: string[];
  /** 由来した学習データセット名 (C 生成時)。 */
  learningSource?: string;
  label?: string;
  model?: string;
  /** C1 実在採用の話者アンカー `ext:<source>:<authorId>` (upsert キー)。 */
  sourceSpeakerId?: string;
  /** Voluptas が HMAC 仮名化した安定 user_id。生 SID は受理しない。 */
  userId?: string;
  /** 母集団平均からの近さ (cosine, 高い=典型)。「平均値グループにいるか」の判断材料。 */
  typicality?: number;
  /** 極性の片寄り |pos-neg|/total (0=均衡 / 1=一方向)。affect 解像度向上 (#125)。 */
  polarityBias?: number;
  /** ゲーム間 affect のばらつき (per-game ベクトルの平均対距離)。affect 解像度向上 (#125)。 */
  affectDispersion?: number;
  preferenceAxes?: Record<string, number>;
  attributes?: { ageBand?: string; spending?: string };
  aversions?: PersonaAversion[];
  mechanicReactions?: PersonaMechanicReaction[];
  exportSpecVersion?: number;
}

function parseJsonArray(s: string): unknown[] {
  try {
    const v = JSON.parse(s);
    return Array.isArray(v) ? v : [];
  } catch {
    return [];
  }
}

function parseJsonObject(s: string): Record<string, unknown> {
  try {
    const value = JSON.parse(s);
    return value && typeof value === "object" && !Array.isArray(value)
      ? value as Record<string, unknown>
      : {};
  } catch {
    return {};
  }
}

interface PersonaRow {
  id: string;
  name: string;
  role: string;
  speech_style: string;
  traits_json: string;
  affect_vector_json: string;
  origin: string;
  parent_ids_json: string;
  learning_source: string | null;
  label: string | null;
  model: string | null;
  source_speaker_id: string | null;
  user_id: string | null;
  typicality: number | null;
  polarity_bias: number | null;
  affect_dispersion: number | null;
  preference_axes_json: string;
  attributes_json: string;
  aversions_json: string;
  mechanic_reactions_json: string;
  export_spec_version: number | null;
}

function rowToPersona(r: PersonaRow): PoolPersona {
  return {
    id: r.id,
    name: r.name,
    role: r.role as FlowRole,
    speechStyle: r.speech_style,
    traits: parseJsonArray(r.traits_json) as string[],
    affectVector: parseJsonArray(r.affect_vector_json) as number[],
    origin: r.origin as PersonaOrigin,
    parentIds: parseJsonArray(r.parent_ids_json) as string[],
    learningSource: r.learning_source ?? undefined,
    label: r.label ?? undefined,
    model: r.model ?? undefined,
    sourceSpeakerId: r.source_speaker_id ?? undefined,
    userId: r.user_id ?? undefined,
    typicality: r.typicality ?? undefined,
    polarityBias: r.polarity_bias ?? undefined,
    affectDispersion: r.affect_dispersion ?? undefined,
    preferenceAxes: parseJsonObject(r.preference_axes_json) as Record<string, number>,
    attributes: parseJsonObject(r.attributes_json) as PoolPersona["attributes"],
    aversions: parseJsonArray(r.aversions_json) as PersonaAversion[],
    mechanicReactions: parseJsonArray(r.mechanic_reactions_json) as PersonaMechanicReaction[],
    exportSpecVersion: r.export_spec_version ?? undefined,
  };
}

/** ペルソナをプールに追加する (id は呼び出し側で採番)。 */
export function insertPoolPersona(p: PoolPersona): void {
  if (p.affectVector.length !== DIM) {
    throw new Error(`affectVector dim must be ${DIM}, got ${p.affectVector.length}`);
  }
  const db = getFlowDb();
  db.prepare(
    `INSERT INTO flow_persona
       (id, name, role, speech_style, traits_json, affect_vector_json, origin, parent_ids_json,
        learning_source, label, model, source_speaker_id, user_id, typicality, polarity_bias, affect_dispersion,
        preference_axes_json, attributes_json, aversions_json, mechanic_reactions_json,
        export_spec_version, archived, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?)`
  ).run(
    p.id,
    p.name,
    p.role,
    p.speechStyle,
    JSON.stringify(p.traits),
    JSON.stringify(p.affectVector),
    p.origin,
    JSON.stringify(p.parentIds),
    p.learningSource ?? null,
    p.label ?? null,
    p.model ?? null,
    p.sourceSpeakerId ?? null,
    p.userId ?? null,
    p.typicality ?? null,
    p.polarityBias ?? null,
    p.affectDispersion ?? null,
    JSON.stringify(p.preferenceAxes ?? {}),
    JSON.stringify(p.attributes ?? {}),
    JSON.stringify(p.aversions ?? []),
    JSON.stringify(p.mechanicReactions ?? []),
    p.exportSpecVersion ?? null,
    Date.now()
  );
}

/**
 * Voluptas の仮名 user_id で取込済みペルソナを引く。
 *
 * `includeArchived` はアーカイブ行も対象にする。 unique index
 * `idx_flow_persona_user_id` は archived を問わず全行に効くので、 upsert の存在判定は
 * これを付けないと「アーカイブ済みの同一人物」を見落として INSERT が制約違反で落ちる。
 */
export function findPoolPersonaByUserId(
  userId: string,
  opts: { includeArchived?: boolean } = {}
): PoolPersona | null {
  const row = getFlowDb()
    .prepare(
      `SELECT * FROM flow_persona WHERE user_id = ?${opts.includeArchived ? "" : " AND archived = 0"}
        ORDER BY created_at ASC LIMIT 1`
    )
    .get(userId) as PersonaRow | undefined;
  return row ? rowToPersona(row) : null;
}

/** Voluptas 由来ペルソナを仮名 user_id 単位で冪等に取り込む (アーカイブ済みは復帰させる)。 */
export function upsertPoolPersonaByUserId(persona: PoolPersona): PoolPersona {
  if (!persona.userId) throw new Error("upsertPoolPersonaByUserId: userId required");
  const existing = findPoolPersonaByUserId(persona.userId, { includeArchived: true })
    ?? (persona.sourceSpeakerId ? findPoolPersonaBySpeaker(persona.sourceSpeakerId) : null);
  if (!existing) {
    insertPoolPersona(persona);
    return persona;
  }
  getFlowDb()
    .prepare(
      `UPDATE flow_persona SET name = ?, role = ?, speech_style = ?, traits_json = ?,
         affect_vector_json = ?, origin = 'imported', learning_source = ?, label = ?, model = ?,
         source_speaker_id = ?, user_id = ?, preference_axes_json = ?, attributes_json = ?,
         aversions_json = ?, mechanic_reactions_json = ?, export_spec_version = ?,
         archived = 0 WHERE id = ?`
    )
    .run(
      persona.name,
      persona.role,
      persona.speechStyle,
      JSON.stringify(persona.traits),
      JSON.stringify(persona.affectVector),
      persona.learningSource ?? "voluptas",
      persona.label ?? null,
      persona.model ?? null,
      persona.sourceSpeakerId ?? existing.sourceSpeakerId ?? null,
      persona.userId,
      JSON.stringify(persona.preferenceAxes ?? {}),
      JSON.stringify(persona.attributes ?? {}),
      JSON.stringify(persona.aversions ?? []),
      JSON.stringify(persona.mechanicReactions ?? []),
      persona.exportSpecVersion ?? null,
      existing.id
    );
  return { ...persona, id: existing.id };
}

/** source_speaker_id (ext:source:authorId) でプールペルソナを引く (C1 採用の upsert キー)。 */
export function findPoolPersonaBySpeaker(sourceSpeakerId: string): PoolPersona | null {
  const r = getFlowDb()
    .prepare(`SELECT * FROM flow_persona WHERE source_speaker_id = ? AND archived = 0 ORDER BY created_at ASC LIMIT 1`)
    .get(sourceSpeakerId) as PersonaRow | undefined;
  return r ? rowToPersona(r) : null;
}

/**
 * 話者アンカー (source_speaker_id) を upsert する (C1)。
 * 既存があれば affect/name/traits/typicality を更新、無ければ insert。返り値は確定ペルソナ。
 */
export function upsertPoolPersonaBySpeaker(p: PoolPersona): PoolPersona {
  if (!p.sourceSpeakerId) throw new Error("upsertPoolPersonaBySpeaker: sourceSpeakerId 必須");
  const existing = findPoolPersonaBySpeaker(p.sourceSpeakerId);
  if (!existing) {
    insertPoolPersona(p);
    return p;
  }
  if (existing.origin === "imported" && existing.userId) {
    getFlowDb()
      .prepare(
        `UPDATE flow_persona SET source_speaker_id = ?, typicality = ?, polarity_bias = ?,
           affect_dispersion = ? WHERE id = ?`
      )
      .run(
        p.sourceSpeakerId,
        p.typicality ?? null,
        p.polarityBias ?? null,
        p.affectDispersion ?? null,
        existing.id
      );
    return {
      ...existing,
      sourceSpeakerId: p.sourceSpeakerId,
      typicality: p.typicality,
      polarityBias: p.polarityBias,
      affectDispersion: p.affectDispersion,
    };
  }
  getFlowDb()
    .prepare(
      `UPDATE flow_persona SET name=?, role=?, speech_style=?, traits_json=?, affect_vector_json=?,
         origin=?, learning_source=?, label=?, typicality=?, polarity_bias=?, affect_dispersion=? WHERE id=?`
    )
    .run(
      p.name,
      p.role,
      p.speechStyle,
      JSON.stringify(p.traits),
      JSON.stringify(p.affectVector),
      p.origin,
      p.learningSource ?? null,
      p.label ?? null,
      p.typicality ?? null,
      p.polarityBias ?? null,
      p.affectDispersion ?? null,
      existing.id
    );
  return { ...p, id: existing.id };
}

/** typicality を更新する (母集団平均確定後の再計算用)。 */
export function setPoolPersonaTypicality(id: string, typicality: number): void {
  getFlowDb().prepare(`UPDATE flow_persona SET typicality = ? WHERE id = ?`).run(typicality, id);
}

/** id でプールペルソナを取得する。 */
export function getPoolPersona(id: string): PoolPersona | null {
  const db = getFlowDb();
  const r = db.prepare(`SELECT * FROM flow_persona WHERE id = ? AND archived = 0`).get(id) as
    | PersonaRow
    | undefined;
  return r ? rowToPersona(r) : null;
}

/** 非アーカイブのプールペルソナを列挙する (origin / learningSource で絞れる)。 */
export function listPoolPersonas(filter?: { origin?: PersonaOrigin; learningSource?: string }): PoolPersona[] {
  const db = getFlowDb();
  const where: string[] = ["archived = 0"];
  const params: unknown[] = [];
  if (filter?.origin) {
    where.push("origin = ?");
    params.push(filter.origin);
  }
  if (filter?.learningSource) {
    where.push("learning_source = ?");
    params.push(filter.learningSource);
  }
  const rows = db
    .prepare(`SELECT * FROM flow_persona WHERE ${where.join(" AND ")} ORDER BY created_at ASC`)
    .all(...params) as PersonaRow[];
  return rows.map(rowToPersona);
}

/**
 * id 完全一致 → name 完全一致の順でプールペルソナを探す (壁打ち相手指定)。
 * 非アーカイブのみ。見つからなければ null。
 */
export function findPoolPersona(idOrName: string): PoolPersona | null {
  const byId = getPoolPersona(idOrName);
  if (byId) return byId;
  const key = idOrName.trim();
  if (!key) return null;
  const r = getFlowDb()
    .prepare(`SELECT * FROM flow_persona WHERE name = ? AND archived = 0 ORDER BY created_at ASC LIMIT 1`)
    .get(key) as PersonaRow | undefined;
  return r ? rowToPersona(r) : null;
}

/** ペルソナをアーカイブする (論理削除)。 */
export function archivePoolPersona(id: string): void {
  getFlowDb().prepare(`UPDATE flow_persona SET archived = 1 WHERE id = ?`).run(id);
}

// ── 嗜好近傍選定 (憑依 / 壁打ち相手) ─────────────────────────────────────────

export interface AffinityHit {
  persona: PoolPersona;
  similarity: number;
}

/**
 * ベクトル最近傍でペルソナを選ぶ (cosine 降順)。
 * candidates 省略時はプール全体 (非アーカイブ) から選ぶ。k 件返す。
 */
export function selectByAffinity(vector: number[], k = 1, candidates?: PoolPersona[]): AffinityHit[] {
  const pool = candidates ?? listPoolPersonas();
  return pool
    .map((persona) => ({ persona, similarity: cosine(vector, persona.affectVector) }))
    .sort((a, b) => b.similarity - a.similarity)
    .slice(0, Math.max(0, k));
}

/**
 * テーマから嗜好/体験を類推して憑依ペルソナを選ぶ (B)。
 * 目標ベクトル = textToVector(theme) (+ 任意でメカニクス affect を加味)。
 * プールが空 / 一致なしなら空配列 → 呼び出し側は従来の生成キャストにフォールバックする。
 */
export function selectPossessionByTheme(theme: string, k = 1, candidates?: PoolPersona[]): AffinityHit[] {
  const vector = textToVector(theme || "");
  return selectByAffinity(vector, k, candidates);
}

/** プールペルソナを議論フローの FlowPersona に変換する (憑依/壁打ち相手として参加させる)。 */
export function toFlowPersona(
  p: PoolPersona,
  opts: { role?: FlowRole; defaultModel: string; isLocal: boolean; stance?: FlowStance }
): FlowPersona {
  const role = opts.role ?? p.role;
  return {
    id: p.id,
    name: p.name,
    role,
    // stance はセッション固定 (respec 01)。指定が無ければロール既定 (debater は pro 起点)。
    stance: opts.stance ?? roleDefaultStance(role),
    speechStyle: p.speechStyle,
    traits: p.traits,
    model: p.model || opts.defaultModel,
    isLocal: opts.isLocal,
  };
}

/**
 * 憑依対象ペルソナの「人物像」を prompt 用の短い説明文にする (B)。
 * C1 採用ペルソナは口調/traits を持たない (affect ベクトルのみ) ため、
 * 出所・典型度・極性偏り・ゲーム間ばらつき等のメタから人物像を言語化する。
 */
export function describePossession(p: PoolPersona): string {
  return buildPersonaDescriptor(p);
}
