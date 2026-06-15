/**
 * ペルソナプール + ユーザ嗜好ベクトル + 嗜好近傍選定 (憑依/合成/壁打ち相手の共通基盤)。
 *
 * - flow_persona: 学習データ別に用意 (C 生成) / 合成 (F) した永続ペルソナ。affect_vector を持つ。
 * - flow_user_affect: ユーザの「ゲームに望む感情/体験」ベクトル (B 憑依の検索キー)。
 * - selectByAffinity: ベクトル最近傍 (cosine) でペルソナを選ぶ = 「憑依」/壁打ち相手の選定。
 *
 * ベクトル空間は sentiment-vector.ts の固定 20 次元 ([[feedback]]: 新空間は作らない)。
 * テキストからのベクトル化は textToVector を流用する。
 */

import { getFlowDb } from "./db/connection.js";
import { cosine, textToVector, DIM } from "./sentiment-vector.js";
import type { FlowPersona, FlowRole } from "./personas.js";

export type PersonaOrigin = "seed" | "generated" | "synthesized" | "adopted";

export interface PoolPersona {
  id: string;
  name: string;
  role: FlowRole;
  speechStyle: string;
  traits: string[];
  /** 固定 20 次元 affect ベクトル。 */
  affectVector: number[];
  origin: PersonaOrigin;
  /** 合成 (synthesized) の親ペルソナ id。 */
  parentIds: string[];
  /** 由来した学習データセット名 (C 生成時)。 */
  learningSource?: string;
  label?: string;
  model?: string;
  /** C1 実在採用の話者アンカー `ext:<source>:<authorId>` (upsert キー)。 */
  sourceSpeakerId?: string;
  /** 母集団平均からの近さ (cosine, 高い=典型)。「平均値グループにいるか」の判断材料。 */
  typicality?: number;
}

export interface UserAffect {
  userKey: string;
  label?: string;
  desiredText: string;
  vector: number[];
  updatedAt: number;
}

function parseJsonArray(s: string): unknown[] {
  try {
    const v = JSON.parse(s);
    return Array.isArray(v) ? v : [];
  } catch {
    return [];
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
  typicality: number | null;
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
    typicality: r.typicality ?? undefined,
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
        learning_source, label, model, source_speaker_id, typicality, archived, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?)`
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
    p.typicality ?? null,
    Date.now()
  );
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
  getFlowDb()
    .prepare(
      `UPDATE flow_persona SET name=?, role=?, speech_style=?, traits_json=?, affect_vector_json=?,
         origin=?, learning_source=?, label=?, typicality=? WHERE id=?`
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
 * id 完全一致 → name 完全一致の順でプールペルソナを探す (壁打ち相手指定 G / 合成 F の親解決)。
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

// ── ユーザ嗜好ベクトル ───────────────────────────────────────────────────────

/** ユーザの「望む体験」テキスト + ベクトルを upsert する。ベクトル未指定なら text から導出。 */
export function upsertUserAffect(args: {
  userKey: string;
  desiredText: string;
  vector?: number[];
  label?: string;
}): UserAffect {
  const vector = args.vector ?? textToVector(args.desiredText);
  if (vector.length !== DIM) throw new Error(`user affect vector dim must be ${DIM}`);
  const updatedAt = Date.now();
  getFlowDb()
    .prepare(
      `INSERT INTO flow_user_affect (user_key, label, desired_text, vector_json, updated_at)
       VALUES (?, ?, ?, ?, ?)
       ON CONFLICT(user_key) DO UPDATE SET
         label = excluded.label,
         desired_text = excluded.desired_text,
         vector_json = excluded.vector_json,
         updated_at = excluded.updated_at`
    )
    .run(args.userKey, args.label ?? null, args.desiredText, JSON.stringify(vector), updatedAt);
  return { userKey: args.userKey, label: args.label, desiredText: args.desiredText, vector, updatedAt };
}

/** ユーザ嗜好を取得する。未登録なら null。 */
export function getUserAffect(userKey: string): UserAffect | null {
  const r = getFlowDb()
    .prepare(`SELECT user_key, label, desired_text, vector_json, updated_at FROM flow_user_affect WHERE user_key = ?`)
    .get(userKey) as
    | { user_key: string; label: string | null; desired_text: string; vector_json: string; updated_at: number }
    | undefined;
  if (!r) return null;
  return {
    userKey: r.user_key,
    label: r.label ?? undefined,
    desiredText: r.desired_text,
    vector: parseJsonArray(r.vector_json) as number[],
    updatedAt: r.updated_at,
  };
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
  opts: { role?: FlowRole; defaultModel: string; isLocal: boolean }
): FlowPersona {
  return {
    id: p.id,
    name: p.name,
    role: opts.role ?? p.role,
    speechStyle: p.speechStyle,
    traits: p.traits,
    model: p.model || opts.defaultModel,
    isLocal: opts.isLocal,
  };
}
