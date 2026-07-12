/**
 * YouTube ライブチャット リプレイ クローラ。
 *
 * yt-dlp でアーカイブ配信の live_chat.json (JSONL) を取得し ExternalUtterance に正規化する。
 * yt-dlp 不要の「ファイル直渡し」モードも提供 (`parseLiveChatFile`)。
 *
 * CLI:
 *   ext-fetch youtube-livechat <gameSlug> <videoId> [--ytdlp <path>]
 *   ext-fetch youtube-livechat <gameSlug> <videoId> --file <live_chat.json>
 *   ext-ingest youtube-livechat <gameSlug> <videoId> [--ytdlp <path>] [--file <path>]
 *
 * yt-dlp インストール: https://github.com/yt-dlp/yt-dlp
 * バイナリパスは env DISCUTERE_YTDLP_PATH または --ytdlp フラグで上書き可能。
 *
 * `mapLiveChatRenderer` / `parseLiveChatJsonl` は純粋関数 (テスト用)。
 */

import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, readdirSync, rmSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";

import type { ExternalUtterance } from "./types.js";

const SOURCE = "youtube-livechat" as const;

// --- raw types (yt-dlp JSONL の 1 行) ---

interface TextRun {
  text?: string;
}
interface EmojiRun {
  emoji?: { emojiId?: string; shortcuts?: string[] };
}
type MessageRun = TextRun | EmojiRun;

export interface LiveChatTextRenderer {
  id?: string;
  timestampUsec?: string;
  authorExternalChannelId?: string;
  authorName?: { simpleText?: string };
  message?: { runs?: MessageRun[] };
}

export interface LiveChatPaidRenderer extends LiveChatTextRenderer {
  purchaseAmountText?: { simpleText?: string };
}

interface LiveChatEvent {
  replayChatItemAction?: {
    actions?: Array<{
      addChatItemAction?: {
        item?: {
          liveChatTextMessageRenderer?: LiveChatTextRenderer;
          liveChatPaidMessageRenderer?: LiveChatPaidRenderer;
        };
      };
    }>;
    videoOffsetTimeMsec?: string;
  };
}

// --- pure helpers ---

function extractText(runs?: MessageRun[]): string {
  if (!runs) return "";
  return runs
    .map((r) => {
      if ("text" in r && r.text) return r.text;
      if ("emoji" in r && r.emoji) {
        return r.emoji.shortcuts?.[0] ?? r.emoji.emojiId ?? "";
      }
      return "";
    })
    .join("");
}

/**
 * liveChatTextMessageRenderer / liveChatPaidMessageRenderer → ExternalUtterance (純粋関数)。
 * 本文が空のメッセージ (スタンプのみ等) は null を返す。
 */
export function mapLiveChatRenderer(
  renderer: LiveChatTextRenderer,
  videoId: string,
  gameSlug: string,
  offsetMs: number,
  isPaid = false
): ExternalUtterance | null {
  const id = renderer.id;
  const channelId = renderer.authorExternalChannelId;
  const content = extractText(renderer.message?.runs);
  if (!id || !channelId || !content.trim()) return null;

  const postedAt = renderer.timestampUsec ? Math.floor(Number(renderer.timestampUsec) / 1000) : 0;
  const offsetSec = Math.floor(offsetMs / 1000);

  return {
    source: SOURCE,
    nativeId: `lc_${id}`,
    gameSlug,
    threadKey: videoId,
    content,
    postedAt,
    videoOffsetMs: offsetMs,
    authorId: channelId,
    authorName: renderer.authorName?.simpleText,
    // スパチャは目印として upvotes=0 を付ける
    signal: isPaid ? { upvotes: 0 } : undefined,
    sourceUrl: `https://www.youtube.com/watch?v=${videoId}&t=${offsetSec}`,
  };
}

/**
 * yt-dlp が出力する live_chat.json (JSONL) をパースして ExternalUtterance[] にする (純粋関数)。
 * 解析不能行・ライブチャット以外のイベントはスキップ。
 */
export function parseLiveChatJsonl(raw: string, videoId: string, gameSlug: string): ExternalUtterance[] {
  const out: ExternalUtterance[] = [];
  for (const line of raw.split("\n")) {
    const trimmed = line.trim();
    if (!trimmed) continue;
    let event: LiveChatEvent;
    try {
      event = JSON.parse(trimmed) as LiveChatEvent;
    } catch {
      continue;
    }
    const replay = event.replayChatItemAction;
    if (!replay) continue;
    const offsetMs = Number(replay.videoOffsetTimeMsec ?? "0");
    for (const action of replay.actions ?? []) {
      const item = action.addChatItemAction?.item;
      if (!item) continue;
      const textR = item.liveChatTextMessageRenderer;
      const paidR = item.liveChatPaidMessageRenderer;
      const renderer = textR ?? paidR;
      if (!renderer) continue;
      const u = mapLiveChatRenderer(renderer, videoId, gameSlug, offsetMs, !!paidR);
      if (u) out.push(u);
    }
  }
  return out;
}

// --- file import (yt-dlp 不要) ---

/** 既存の live_chat.json ファイルを直接パースして ExternalUtterance[] にする。 */
export function parseLiveChatFile(filePath: string, videoId: string, gameSlug: string): ExternalUtterance[] {
  return parseLiveChatJsonl(readFileSync(filePath, "utf8"), videoId, gameSlug);
}

// --- yt-dlp runner ---

export interface LiveChatFetchOptions {
  videoId: string;
  gameSlug: string;
  /** yt-dlp バイナリパス。未指定なら env DISCUTERE_YTDLP_PATH → "yt-dlp" の順で探す。 */
  ytdlpPath?: string;
  onProgress?: (msg: string) => void;
}

/**
 * yt-dlp でライブチャット リプレイを取得して ExternalUtterance[] にする。
 * ライブチャットが存在しない動画では空配列を返す (エラーにしない)。
 */
export function fetchLiveChatReplay(opts: LiveChatFetchOptions): ExternalUtterance[] {
  const ytdlp = opts.ytdlpPath ?? process.env.DISCUTERE_YTDLP_PATH ?? "yt-dlp";
  const dir = mkdtempSync(join(tmpdir(), "di-livechat-"));
  try {
    const url = `https://www.youtube.com/watch?v=${opts.videoId}`;
    opts.onProgress?.(`yt-dlp 実行中 (${opts.videoId})...`);
    const result = spawnSync(
      ytdlp,
      ["--skip-download", "--write-subs", "--sub-langs", "live_chat", "-o", "%(id)s.%(ext)s", url],
      { cwd: dir, encoding: "utf8", timeout: 120_000 }
    );
    if (result.error) {
      throw new Error(
        `yt-dlp の起動に失敗しました。インストールを確認してください: https://github.com/yt-dlp/yt-dlp\n${result.error.message}`
      );
    }
    const files = readdirSync(dir).filter((f) => f.endsWith(".live_chat.json"));
    if (files.length === 0) {
      opts.onProgress?.("ライブチャットが見つかりません (通常コメントのみの動画の可能性があります)");
      return [];
    }
    const items = parseLiveChatJsonl(readFileSync(join(dir, files[0]), "utf8"), opts.videoId, opts.gameSlug);
    opts.onProgress?.(`${items.length} 件取得`);
    return items;
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}
