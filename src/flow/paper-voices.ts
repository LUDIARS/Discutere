/**
 * ディスカッションペーパーの声の保存と検索 (spec/feature/flow/paper-reuse.md)。
 *
 * ペーパー草案を作るときに集めた声 (外部の声 + ユーザーの声の均等混合) をセッションごとに保存し、
 *   - 別の議論でペーパーを使い回すとき (paper-reuse.ts)
 *   - 再起動でメモリ上の声の検索を失ったスレッドのやり直し
 * で、集め直さずに同じ声を議論に渡す。保存するのは本文・出所・元 URL だけ (個人アンカーは持たない)。
 */

import { getFlowDb } from "./db/connection.js";
import type { ContextVoice } from "./discussion-paper.js";
import { extractKeyTerms } from "../discatier-engine-adapter/keyword-terms.js";

/** 1 ペーパーに保存する声の上限。 */
export const PAPER_VOICE_LIMIT = 200;

export type VoiceLookup = (terms: string[], limit: number) => ContextVoice[];

/** セッションの声を保存する (置き換え。空なら何もしない = 既存を消さない)。 */
export function savePaperVoices(sessionId: string, voices: readonly ContextVoice[]): number {
  const rows = voices.filter((v) => v.content?.trim()).slice(0, PAPER_VOICE_LIMIT);
  if (rows.length === 0) return 0;
  const db = getFlowDb();
  const now = Date.now();
  const insert = db.prepare(
    `INSERT INTO discussion_paper_voice (session_id, ordinal, content, source, source_url, created_at)
     VALUES (?, ?, ?, ?, ?, ?)`
  );
  db.transaction(() => {
    db.prepare(`DELETE FROM discussion_paper_voice WHERE session_id = ?`).run(sessionId);
    rows.forEach((v, i) => insert.run(sessionId, i, v.content, v.source, v.sourceUrl ?? null, now));
  })();
  return rows.length;
}

/** セッションに保存した声 (保存順)。 */
export function loadPaperVoices(sessionId: string): ContextVoice[] {
  const rows = getFlowDb()
    .prepare(
      `SELECT content, source, source_url FROM discussion_paper_voice WHERE session_id = ? ORDER BY ordinal`
    )
    .all(sessionId) as Array<{ content: string; source: string; source_url: string | null }>;
  return rows.map((r) => ({ content: r.content, source: r.source, ...(r.source_url ? { sourceUrl: r.source_url } : {}) }));
}

/**
 * 保存した声から検索する。検索語を多く含む声を先に、同点は保存順で返す
 * (一致が無くても保存した声を返す = 使い回した声を必ず議論に渡す)。
 */
export function storedVoiceLookup(voices: readonly ContextVoice[]): VoiceLookup {
  return (terms, limit) => {
    const keys = extractKeyTerms(terms);
    const scored = voices.map((v, i) => {
      const text = v.content.toLowerCase();
      return { v, i, score: keys.filter((k) => text.includes(k)).length };
    });
    scored.sort((a, b) => b.score - a.score || a.i - b.i);
    return scored.slice(0, Math.max(0, limit)).map((s) => s.v);
  };
}

/** セッションに保存した声があればその検索、無ければ fallback。 */
export function sessionVoiceLookup(sessionId: string, fallback?: VoiceLookup): VoiceLookup | undefined {
  const stored = loadPaperVoices(sessionId);
  return stored.length > 0 ? storedVoiceLookup(stored) : fallback;
}
