/** Discord スレッドと現在の議論を対応付ける。再議論でも以前のペーパー・発言・結論を保持する。 */
import { randomUUID } from "node:crypto";
import { getFlowDb } from "./db/connection.js";
import { getPaperSnapshot, persistDraftPaper } from "./discussion-paper.js";
import { stripProgress } from "./paper-markdown.js";
import { appendRevision } from "./paper-revisions.js";

export function forumSessionId(threadId: string): string {
  const row = getFlowDb().prepare("SELECT session_id FROM discord_flow_run WHERE thread_id = ?").get(threadId) as { session_id: string } | undefined;
  return row?.session_id ?? threadId;
}

/** 修正用の新しい草案を作る。再度の呼び出しは同じ草案を返す。 */
export function openRediscussion(threadId: string): string | null {
  const db = getFlowDb();
  return db.transaction(() => {
    const previous = forumSessionId(threadId);
    const paper = getPaperSnapshot(previous);
    if (!paper || !["discussion", "improvement"].includes(paper.flow)) return null;
    if (paper.status === "draft") return previous;
    const sessionId = `rediscussion-${randomUUID()}`;
    const bodyMd = stripProgress(paper.bodyMd);
    persistDraftPaper({ sessionId, theme: paper.theme, tags: paper.tags, mechanics: paper.mechanics, supplement: paper.supplement, bodyMd }, paper.flow, { originUi: "discord" });
    appendRevision({ sessionId, bodyMd, changeSummary: "前回のペーパーから再議論の草案を作成", origin: "initial" });
    db.prepare("INSERT INTO discord_flow_run (thread_id, session_id, previous_session_id) VALUES (?, ?, ?) ON CONFLICT(thread_id) DO UPDATE SET session_id=excluded.session_id, previous_session_id=excluded.previous_session_id").run(threadId, sessionId, previous);
    return sessionId;
  })();
}
