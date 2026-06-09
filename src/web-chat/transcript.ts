/**
 * Web チャット room の会話読み出し。
 *
 * 1 つの room (scene=web:<roomId>) には 2 種類の session がぶら下がる:
 *   - 入口 session (`web-session:<roomId>`)        … 人間の平文発話
 *   - 議論 session (`discussion-of-gap:<gapId>`)   … persona / facilitator の発話
 *                                                     (+ event-bridge がミラーした人間発話)
 *
 * 人間発話は入口と議論の両方に載る (ミラー) ため、 表示では二重計上を避ける:
 *   - 人間発話   → 入口 session 由来のものだけ採用
 *   - 非人間発話 → 議論 session 由来のものを採用 (人間ミラーは除外)
 *
 * 個人アンカーは保存していない (匿名 workspace)。 speaker_id の prefix で役割を判定する。
 */

import type { createCore } from "../core/index.js";
import { webScene } from "./session.js";

type Core = ReturnType<typeof createCore>;

export type ChatRole = "human" | "persona" | "external" | "facilitator";

export interface ChatMessage {
  id: string;
  role: ChatRole;
  /** 表示名 (persona は display_name 解決済 / 人間は author / 外部は「外部の声」)。 */
  speaker: string;
  content: string;
  postedAt: number;
}

/** persona id → 表示名を解決する (未解決なら id をそのまま)。index.ts から注入する。 */
export type SpeakerNameResolver = (personaId: string) => string;

interface UtteranceRow {
  id: string;
  speakerId: string | null;
  content: string;
  postedAt: number;
  sessionTitle: string;
}

export interface ReadTranscriptOptions {
  /** この時刻 (ms) 以降の発話だけ返す (増分取得用、 既定 0 = 全件)。 */
  sinceTs?: number;
  /** 返す最大件数 (既定 500)。 */
  limit?: number;
  resolveName?: SpeakerNameResolver;
}

function classifyRole(speakerId: string | null): ChatRole {
  if (!speakerId) return "facilitator";
  if (speakerId.startsWith("persona:")) return "persona";
  if (speakerId.startsWith("ext:")) return "external";
  return "human";
}

/**
 * scene=web:<roomId> に属する全 session の発話を時系列 (古い順) で 1 本の会話として返す。
 */
export function readRoomTranscript(
  core: Core,
  workspaceId: string,
  roomId: string,
  options: ReadTranscriptOptions = {}
): ChatMessage[] {
  const since = options.sinceTs ?? 0;
  const limit = options.limit ?? 500;
  const rows = core.client.raw
    .prepare(
      `SELECT u.id AS id, u.speaker_id AS speakerId, u.raw_content AS content,
              u.posted_at AS postedAt, s.title AS sessionTitle
         FROM utterances u
         JOIN sessions s ON s.id = u.session_id
        WHERE s.workspace_id = ?
          AND s.scene = ?
          AND u.posted_at >= ?
        ORDER BY u.posted_at ASC, u.id ASC
        LIMIT ?`
    )
    .all(workspaceId, webScene(roomId), since, limit) as UtteranceRow[];

  const messages: ChatMessage[] = [];
  for (const r of rows) {
    const role = classifyRole(r.speakerId);
    const fromInbox = r.sessionTitle.startsWith("web-session:");
    // 人間発話は入口 session 由来のみ採用 (議論 session のミラー重複を除外)。
    if (role === "human" && !fromInbox) continue;
    // 非人間発話は議論 session 由来のみ (入口 session に非人間が載ることは無いが念のため)。
    if (role !== "human" && fromInbox) continue;
    messages.push({
      id: r.id,
      role,
      speaker: resolveSpeaker(role, r.speakerId, options.resolveName),
      content: r.content,
      postedAt: r.postedAt,
    });
  }
  return messages;
}

function resolveSpeaker(
  role: ChatRole,
  speakerId: string | null,
  resolveName?: SpeakerNameResolver
): string {
  if (role === "persona") {
    const pid = (speakerId ?? "").slice("persona:".length);
    return resolveName ? resolveName(pid) : pid || "論者";
  }
  if (role === "external") return "外部の声";
  if (role === "facilitator") return "進行役";
  return speakerId || "あなた";
}
