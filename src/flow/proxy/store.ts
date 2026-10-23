/**
 * 代理議論の人物パックの永続 (flow_proxy_profile)。
 *
 * 1 行 = 1 話者 × 1 ジャンル。CRUD と「キャストに出してよいか」の読み出し条件だけを持つ
 * (取込の判断は import.ts、束の作り方は bundle.ts)。露出名 label は speakerKey の一方向 hash で、
 * 生の外部 ID はこの表に存在しない (契約が仮名しか通さない)。
 */

import { getFlowDb } from "../db/connection.js";
import type { Confidence, PersonaPack } from "./contract.js";

export type ProxyEvalStatus = "unevaluated" | "passed" | "failed";

export interface ProxyProfileRow {
  speakerKey: string;
  genre: string;
  label: string;
  profileVersion: string;
  contentHash: string;
  pack: PersonaPack;
  confidence: Confidence;
  expiresAt: number;
  revoked: boolean;
  evalStatus: ProxyEvalStatus;
  evalJson: string | null;
  importedAt: number;
  updatedAt: number;
}

interface RawRow {
  speaker_key: string;
  genre: string;
  label: string;
  profile_version: string;
  content_hash: string;
  pack_json: string;
  confidence: string;
  expires_at: number;
  revoked: number;
  eval_status: string;
  eval_json: string | null;
  imported_at: number;
  updated_at: number;
}

function toRow(r: RawRow): ProxyProfileRow {
  return {
    speakerKey: r.speaker_key,
    genre: r.genre,
    label: r.label,
    profileVersion: r.profile_version,
    contentHash: r.content_hash,
    pack: JSON.parse(r.pack_json) as PersonaPack,
    confidence: r.confidence as Confidence,
    expiresAt: r.expires_at,
    revoked: r.revoked === 1,
    evalStatus: r.eval_status as ProxyEvalStatus,
    evalJson: r.eval_json,
    importedAt: r.imported_at,
    updatedAt: r.updated_at,
  };
}

export function getProxyProfile(speakerKey: string, genre: string): ProxyProfileRow | null {
  const row = getFlowDb()
    .prepare(`SELECT * FROM flow_proxy_profile WHERE speaker_key = ? AND genre = ?`)
    .get(speakerKey, genre) as RawRow | undefined;
  return row ? toRow(row) : null;
}

export interface UpsertProxyProfileInput {
  pack: PersonaPack;
  label: string;
  contentHash: string;
  now: number;
}

/**
 * パックを upsert する。内容 hash が変わったときだけ評価状態を unevaluated に戻す
 * (同じ内容の再取込で合否を消さない)。
 */
export function upsertProxyProfile(input: UpsertProxyProfileInput): "inserted" | "updated" | "unchanged" {
  const { pack, label, contentHash, now } = input;
  const existing = getProxyProfile(pack.speakerKey, pack.genre);
  if (existing && existing.contentHash === contentHash && !existing.revoked) return "unchanged";
  const db = getFlowDb();
  if (!existing) {
    db.prepare(
      `INSERT INTO flow_proxy_profile
         (speaker_key, genre, label, profile_version, content_hash, pack_json, confidence, expires_at,
          revoked, eval_status, eval_json, imported_at, updated_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, 'unevaluated', NULL, ?, ?)`
    ).run(
      pack.speakerKey,
      pack.genre,
      label,
      pack.profileVersion,
      contentHash,
      JSON.stringify(pack),
      pack.sufficiency.confidence,
      pack.window.expiresAt,
      now,
      now
    );
    return "inserted";
  }
  db.prepare(
    `UPDATE flow_proxy_profile
        SET label = ?, profile_version = ?, content_hash = ?, pack_json = ?, confidence = ?, expires_at = ?,
            revoked = 0, eval_status = 'unevaluated', eval_json = NULL, updated_at = ?
      WHERE speaker_key = ? AND genre = ?`
  ).run(
    label,
    pack.profileVersion,
    contentHash,
    JSON.stringify(pack),
    pack.sufficiency.confidence,
    pack.window.expiresAt,
    now,
    pack.speakerKey,
    pack.genre
  );
  return "updated";
}

/** 失効 (形成側の revoked / オプトアウト)。行は残して以後のキャストから外す。 */
export function revokeProxyProfile(speakerKey: string, genre: string, now: number): boolean {
  const r = getFlowDb()
    .prepare(`UPDATE flow_proxy_profile SET revoked = 1, updated_at = ? WHERE speaker_key = ? AND genre = ? AND revoked = 0`)
    .run(now, speakerKey, genre);
  return r.changes > 0;
}

export function setProxyEvalStatus(speakerKey: string, genre: string, status: ProxyEvalStatus, evalJson: string, now: number): void {
  getFlowDb()
    .prepare(`UPDATE flow_proxy_profile SET eval_status = ?, eval_json = ?, updated_at = ? WHERE speaker_key = ? AND genre = ?`)
    .run(status, evalJson, now, speakerKey, genre);
}

/**
 * キャストに出してよい行か (失効・期限切れ・低信頼・評価不合格を除く)。
 * 評価前 (unevaluated) は requirePassed=true のときだけ除く。
 */
export function isCastable(row: ProxyProfileRow, now: number, requirePassed: boolean): string | null {
  if (row.revoked) return "失効している";
  if (row.expiresAt <= now) return "有効期限が切れている";
  if (row.confidence === "low") return "材料が足りない (confidence=low)";
  if (row.evalStatus === "failed") return "本人らしさの評価に合格していない";
  if (requirePassed && row.evalStatus !== "passed") return "本人らしさの評価がまだ";
  return null;
}

/** 露出名 (論者#xxxxxx) で引く。genre 指定が無ければ同じ人物の全ジャンル。 */
export function findProxyProfilesByLabel(label: string, genre?: string): ProxyProfileRow[] {
  const db = getFlowDb();
  const rows = (
    genre
      ? db.prepare(`SELECT * FROM flow_proxy_profile WHERE label = ? AND genre = ? ORDER BY updated_at DESC`).all(label, genre)
      : db.prepare(`SELECT * FROM flow_proxy_profile WHERE label = ? ORDER BY updated_at DESC`).all(label)
  ) as RawRow[];
  return rows.map(toRow);
}

export function listProxyProfiles(opts: { genre?: string; includeRevoked?: boolean } = {}): ProxyProfileRow[] {
  const where: string[] = [];
  const params: unknown[] = [];
  if (opts.genre) {
    where.push("genre = ?");
    params.push(opts.genre);
  }
  if (!opts.includeRevoked) where.push("revoked = 0");
  const sql = `SELECT * FROM flow_proxy_profile${where.length ? ` WHERE ${where.join(" AND ")}` : ""} ORDER BY genre, label`;
  return (getFlowDb().prepare(sql).all(...params) as RawRow[]).map(toRow);
}
