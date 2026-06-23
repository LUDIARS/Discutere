/**
 * 議論意見へのリアクション → 内部スコアリング (spec/feature/facilitator/DESIGN.md)。
 *
 * - Discord に投稿した persona 発話 / 仮説の message_id ↔ 対象(utterance/hypothesis) を記録
 * - その message にリアクションが付いたら、 絵文字ごとの重みで対象のスコアを加算
 * - ファシリテーターのまとめはこのスコアに引っ張られる (高スコア意見を優先採用)
 *
 * テーブルは KG (core.client.raw / better-sqlite3) に同居させる (schema 変更は単純な CREATE)。
 */

import type DatabaseType from "better-sqlite3";

type RawDb = DatabaseType.Database;

export type OpinionKind = "utterance" | "hypothesis";

/** 絵文字ごとの加点重み (既定)。 未知の絵文字は 1。 */
export const DEFAULT_REACTION_WEIGHTS: Record<string, number> = {
  "👍": 1,
  "❤️": 2,
  "🔥": 2,
  "✅": 2,
  "🎯": 2,
  "💡": 3,
  "💯": 3,
  "⭐": 2,
};

export function reactionWeight(
  emoji: string,
  weights: Record<string, number> = DEFAULT_REACTION_WEIGHTS
): number {
  return weights[emoji] ?? 1;
}

export function ensureReactionTables(db: RawDb): void {
  db.exec(
    `CREATE TABLE IF NOT EXISTS discord_message_map (
       message_id TEXT PRIMARY KEY,
       target_id TEXT NOT NULL,
       target_kind TEXT NOT NULL,
       channel_id TEXT,
       created_at INTEGER NOT NULL
     );
     CREATE TABLE IF NOT EXISTS opinion_scores (
       target_id TEXT PRIMARY KEY,
       target_kind TEXT NOT NULL,
       score INTEGER NOT NULL DEFAULT 0,
       updated_at INTEGER NOT NULL
     );`
  );
}

/** 投稿した Discord message と対象意見の対応を記録。 */
export function recordPostedMessage(
  db: RawDb,
  input: { messageId: string; targetId: string; targetKind: OpinionKind; channelId?: string; at?: number }
): void {
  if (!input.messageId) return;
  db.prepare(
    "INSERT OR REPLACE INTO discord_message_map (message_id, target_id, target_kind, channel_id, created_at) VALUES (?, ?, ?, ?, ?)"
  ).run(input.messageId, input.targetId, input.targetKind, input.channelId ?? null, input.at ?? Date.now());
}

/** リアクションを 1 件適用してスコア加算。 対象が見つかれば結果を返す。 */
export function applyReaction(
  db: RawDb,
  input: { messageId: string; emoji: string; weights?: Record<string, number>; at?: number }
): { targetId: string; targetKind: OpinionKind; weight: number; score: number } | null {
  const row = db
    .prepare("SELECT target_id, target_kind FROM discord_message_map WHERE message_id = ?")
    .get(input.messageId) as { target_id: string; target_kind: OpinionKind } | undefined;
  if (!row) return null;
  const weight = reactionWeight(input.emoji, input.weights);
  const at = input.at ?? Date.now();
  db.prepare(
    `INSERT INTO opinion_scores (target_id, target_kind, score, updated_at)
       VALUES (?, ?, ?, ?)
     ON CONFLICT(target_id) DO UPDATE SET score = score + excluded.score, updated_at = excluded.updated_at`
  ).run(row.target_id, row.target_kind, weight, at);
  const cur = db.prepare("SELECT score FROM opinion_scores WHERE target_id = ?").get(row.target_id) as
    | { score: number }
    | undefined;
  return { targetId: row.target_id, targetKind: row.target_kind, weight, score: cur?.score ?? weight };
}

export function getOpinionScore(db: RawDb, targetId: string): number {
  const row = db.prepare("SELECT score FROM opinion_scores WHERE target_id = ?").get(targetId) as
    | { score: number }
    | undefined;
  return row?.score ?? 0;
}

/** session 内の高スコア意見を上位から (まとめがスコアに引っ張られる用)。 */
export function topScoredOpinions(
  db: RawDb,
  utteranceIds: string[],
  limit: number
): Array<{ targetId: string; score: number }> {
  return utteranceIds
    .map((id) => ({ targetId: id, score: getOpinionScore(db, id) }))
    .filter((x) => x.score > 0)
    .sort((a, b) => b.score - a.score)
    .slice(0, limit);
}
