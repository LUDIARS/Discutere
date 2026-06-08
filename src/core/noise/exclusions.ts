/**
 * 発話ノイズ除外 (per-utterance soft-exclude)。
 *
 * 別ゲームの雑談 / 人間の煽り等を「結論データ・データソースから外したい」要求への対応。
 * utterances テーブルには触らず (ALTER しない)、サイドカー表 utterance_exclusions に
 * 除外 id を積む。除外は可逆 (restore で DELETE)、まとめ生成・読み取り側は
 * `id NOT IN (SELECT utterance_id FROM utterance_exclusions)` で弾く。
 *
 * これにより既存データを破壊せず「ノイズだけ除いた結論」を再生成できる。
 */

import type Database from "better-sqlite3";

type Raw = Database.Database;

/** WHERE 句で再利用する除外フィルタの定数 SQL フラグメント。 */
export const EXCLUDED_NOT_IN_SQL =
  "id NOT IN (SELECT utterance_id FROM utterance_exclusions)";

/** サイドカー表を冪等に確保する。 */
export function ensureExclusionTable(raw: Raw): void {
  raw.exec(
    `CREATE TABLE IF NOT EXISTS utterance_exclusions (
       utterance_id TEXT PRIMARY KEY,
       reason TEXT,
       created_at INTEGER NOT NULL
     )`
  );
}

/** 発話をノイズとして除外する (INSERT OR REPLACE で冪等)。 */
export function excludeUtterance(raw: Raw, utteranceId: string, reason?: string): void {
  ensureExclusionTable(raw);
  raw
    .prepare(
      "INSERT OR REPLACE INTO utterance_exclusions (utterance_id, reason, created_at) VALUES (?, ?, ?)"
    )
    .run(utteranceId, reason ?? null, Date.now());
}

/** 除外を取り消す (元に戻す)。 */
export function restoreUtterance(raw: Raw, utteranceId: string): void {
  ensureExclusionTable(raw);
  raw.prepare("DELETE FROM utterance_exclusions WHERE utterance_id = ?").run(utteranceId);
}

/** 指定発話が除外済みか。 */
export function isExcluded(raw: Raw, utteranceId: string): boolean {
  ensureExclusionTable(raw);
  const row = raw
    .prepare("SELECT 1 AS x FROM utterance_exclusions WHERE utterance_id = ?")
    .get(utteranceId) as { x: number } | undefined;
  return !!row;
}

/** 除外済み id の集合を返す。 */
export function listExcludedIds(raw: Raw): Set<string> {
  ensureExclusionTable(raw);
  const rows = raw
    .prepare("SELECT utterance_id FROM utterance_exclusions")
    .all() as Array<{ utterance_id: string }>;
  return new Set(rows.map((r) => r.utterance_id));
}
