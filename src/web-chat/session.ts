/**
 * Web チャット トランスポートの session 管理 (Discord の `ensureDiscordSession` / forum ヘルパの web 版)。
 *
 * Di の議論は `scene` 文字列でトランスポートを束ねる。 Discord は `discord:<guild>/<channel>`、
 * Web チャットは `web:<roomId>` を使う。 入口の人間発話 session・進行中議論 session ともに
 * 同じ scene を持たせることで、 event-bridge が議論への scene 継承 / 人間発話ミラーを行い、
 * 1 つの room を 1 つの会話として読み書きできる (`transcript.ts`)。
 *
 * 個人データは保存しない (匿名 workspace 方針)。 author は表示名のみで personId に載せる。
 */

import type { createCore } from "../core/index.js";

type Core = ReturnType<typeof createCore>;

/** web room に対応する scene 文字列。 */
export function webScene(roomId: string): string {
  return `web:${roomId}`;
}

/** roomId を session/scene に使える安全な文字列に正規化する (英数 . _ - のみ、 32 文字)。 */
export function sanitizeRoomId(raw: string | undefined | null): string {
  const v = (raw ?? "").trim().replace(/[^A-Za-z0-9._-]/g, "");
  return v.length > 0 ? v.slice(0, 32) : "lobby";
}

/**
 * web room の入口 session を 1 つ確保する (なければ作る)。
 * 人間の平文発話はここに記録され、 UtteranceCreated → event-bridge が議論にミラーする。
 */
export function ensureWebSession(core: Core, workspaceId: string, roomId: string): string {
  const title = `web-session:${roomId}`;
  const existing = core.client.raw
    .prepare(
      "SELECT id FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1"
    )
    .get(workspaceId, title) as { id: string } | undefined;
  if (existing) return existing.id;
  return core.repos.session.create({
    workspaceId,
    title,
    startedAt: Date.now(),
    scene: webScene(roomId),
  } as never);
}

/**
 * 当該 room (scene=web:<roomId>) で open な議論 session が既にあるか。
 * `command-router.forumDiscussionExists` の web 版 — 二重起動防止 / 状態表示に使う。
 */
export function webDiscussionExists(core: Core, workspaceId: string, roomId: string): boolean {
  const row = core.client.raw
    .prepare(
      `SELECT s.id AS id
         FROM sessions s
         JOIN design_gaps g
           ON g.id = SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1)
        WHERE s.workspace_id = ?
          AND s.title LIKE 'discussion-of-gap:%'
          AND s.scene = ?
          AND (g.status IS NULL OR g.status NOT IN ('closed', 'converged', 'dismissed'))
        LIMIT 1`
    )
    .get(workspaceId, webScene(roomId)) as { id: string } | undefined;
  return !!row;
}
