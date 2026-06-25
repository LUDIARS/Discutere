/**
 * ディスカッションペーパー本文 (markdown) の **版履歴ストア** (Notion 風編集の「戻す」基盤)。
 *
 * Web の編集ゲートでは、初期草案 → ブロック編集 / 自然文調整 / クロール反映 のたびに
 * 本文 md を 1 リビジョンとして追記する。「戻す」は履歴を削除せず、1 手前の本文を
 * **新しいリビジョンとして積み直す** (前進専用 = 監査ログを失わない)。
 *
 * session 単位で管理し、承認前 (永続前) の可変ドラフトでも復元できるよう DB に置く
 * (サーバ再起動を跨いでも編集途中のペーパーが残る)。discutere.db に追記する。
 */

import { getFlowDb } from "./db/connection.js";

/** 本文 md がどの操作で生まれたか (監査 / UI 表示用)。 */
export type RevisionOrigin = "initial" | "llm-edit" | "crawl" | "manual" | "revert";

export interface PaperRevision {
  rev: number;
  bodyMd: string;
  changeSummary: string;
  origin: RevisionOrigin;
  createdAt: number;
}

/** session の最新リビジョン番号を返す (無ければ 0)。 */
function latestRev(sessionId: string): number {
  const row = getFlowDb()
    .prepare(`SELECT MAX(rev) AS maxRev FROM discussion_paper_revision WHERE session_id = ?`)
    .get(sessionId) as { maxRev: number | null } | undefined;
  return row?.maxRev ?? 0;
}

/**
 * 本文 md を 1 リビジョン追記し、その rev 番号を返す。
 * 直前のリビジョンと本文が同一なら追記しない (no-op で既存 rev を返す)。
 */
export function appendRevision(args: {
  sessionId: string;
  bodyMd: string;
  changeSummary: string;
  origin: RevisionOrigin;
}): number {
  const db = getFlowDb();
  const current = latestRevision(args.sessionId);
  if (current && current.bodyMd === args.bodyMd) return current.rev;
  const rev = latestRev(args.sessionId) + 1;
  db.prepare(
    `INSERT INTO discussion_paper_revision (session_id, rev, body_md, change_summary, origin, created_at)
     VALUES (?, ?, ?, ?, ?, ?)`
  ).run(args.sessionId, rev, args.bodyMd, args.changeSummary, args.origin, Date.now());
  return rev;
}

/** session の最新リビジョンを返す (無ければ null)。 */
export function latestRevision(sessionId: string): PaperRevision | null {
  const row = getFlowDb()
    .prepare(
      `SELECT rev, body_md, change_summary, origin, created_at
         FROM discussion_paper_revision
        WHERE session_id = ?
        ORDER BY rev DESC LIMIT 1`
    )
    .get(sessionId) as
    | { rev: number; body_md: string; change_summary: string; origin: string; created_at: number }
    | undefined;
  if (!row) return null;
  return {
    rev: row.rev,
    bodyMd: row.body_md,
    changeSummary: row.change_summary,
    origin: row.origin as RevisionOrigin,
    createdAt: row.created_at,
  };
}

/** session の全リビジョンを古い順で返す。 */
export function listRevisions(sessionId: string): PaperRevision[] {
  const rows = getFlowDb()
    .prepare(
      `SELECT rev, body_md, change_summary, origin, created_at
         FROM discussion_paper_revision
        WHERE session_id = ?
        ORDER BY rev ASC`
    )
    .all(sessionId) as Array<{
    rev: number;
    body_md: string;
    change_summary: string;
    origin: string;
    created_at: number;
  }>;
  return rows.map((r) => ({
    rev: r.rev,
    bodyMd: r.body_md,
    changeSummary: r.change_summary,
    origin: r.origin as RevisionOrigin,
    createdAt: r.created_at,
  }));
}

/** 1 手前に戻せるか (リビジョンが 2 つ以上あるか)。 */
export function canRevert(sessionId: string): boolean {
  return latestRev(sessionId) >= 2;
}

/**
 * 1 手前の本文を新リビジョンとして積み直す (= 「戻す」)。
 * 戻せない (履歴が 1 つ以下) 場合は null。
 */
export function revertLast(sessionId: string): PaperRevision | null {
  const all = listRevisions(sessionId);
  if (all.length < 2) return null;
  const previous = all[all.length - 2];
  const rev = appendRevision({
    sessionId,
    bodyMd: previous.bodyMd,
    changeSummary: `1 手前 (rev ${previous.rev}) に戻しました`,
    origin: "revert",
  });
  return latestRevision(sessionId) ?? { ...previous, rev, origin: "revert" };
}
