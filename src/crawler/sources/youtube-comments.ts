/**
 * YouTube コメントクローラ — spec/feature/crawler/EXTERNAL-SOURCES.md §4.2(b)。
 *
 * 動画 id → コメント本文。commentThreads.list (1 unit/100 thread) でトップレベル +
 * 一部返信を取得し、ExternalUtterance に正規化する (返信は responds_to=トップコメント)。
 * コメント無効動画は空配列を返して skip。
 *
 * `mapYoutubeComment` / `mapCommentThread` は純粋関数 (テスト用)。
 */

import { YT_COST, type QuotaTracker } from "./youtube-quota.js";
import type { ExternalUtterance } from "./types.js";

const API_BASE = "https://www.googleapis.com/youtube/v3";
const SOURCE = "youtube" as const;

export interface YoutubeCommentRaw {
  id: string;
  snippet?: {
    textOriginal?: string;
    authorDisplayName?: string;
    authorChannelId?: { value?: string };
    publishedAt?: string;
    likeCount?: number;
  };
}

export interface CommentThreadRaw {
  snippet?: {
    topLevelComment?: YoutubeCommentRaw;
    totalReplyCount?: number;
  };
  replies?: { comments?: YoutubeCommentRaw[] };
}

/** YouTube comment 1 件 → ExternalUtterance (純粋関数)。 */
export function mapYoutubeComment(
  c: YoutubeCommentRaw,
  videoId: string,
  gameSlug: string,
  parentNativeId?: string
): ExternalUtterance {
  const channelId = c.snippet?.authorChannelId?.value ?? "unknown";
  return {
    source: SOURCE,
    nativeId: c.id,
    gameSlug,
    threadKey: videoId,
    content: c.snippet?.textOriginal ?? "",
    postedAt: c.snippet?.publishedAt ? Date.parse(c.snippet.publishedAt) : 0,
    authorId: channelId, // channelId(UCxxxx) = 公開・安定な persona アンカー
    authorName: c.snippet?.authorDisplayName,
    parentNativeId,
    signal: { upvotes: c.snippet?.likeCount },
    sourceUrl: `https://www.youtube.com/watch?v=${videoId}&lc=${c.id}`,
  };
}

/** commentThreads.list の 1 thread → [トップコメント, ...返信] (親→子順)。 */
export function mapCommentThread(
  thread: CommentThreadRaw,
  videoId: string,
  gameSlug: string
): ExternalUtterance[] {
  const top = thread.snippet?.topLevelComment;
  if (!top) return [];
  const out: ExternalUtterance[] = [mapYoutubeComment(top, videoId, gameSlug)];
  for (const reply of thread.replies?.comments ?? []) {
    out.push(mapYoutubeComment(reply, videoId, gameSlug, top.id));
  }
  return out;
}

export interface YoutubeCommentsOptions {
  videoId: string;
  gameSlug: string;
  apiKey: string;
  /** 取得コメント数の上限 (top+replies 合算)。0/undefined で無制限 */
  maxComments?: number;
  /** relevance | time。既定 relevance */
  order?: string;
  quota?: QuotaTracker;
  fetchImpl?: typeof fetch;
  onPage?: (total: number) => void;
}

function isCommentsDisabled(body: string): boolean {
  return /commentsDisabled|disabled comments/i.test(body);
}

/** 動画 1 本のコメントを取得して ExternalUtterance[] にする。 */
export async function fetchVideoComments(opts: YoutubeCommentsOptions): Promise<ExternalUtterance[]> {
  const doFetch = opts.fetchImpl ?? fetch;
  const order = opts.order ?? "relevance";
  const out: ExternalUtterance[] = [];
  let pageToken: string | undefined;

  for (;;) {
    if (opts.quota && !opts.quota.canSpend(YT_COST.list)) break;
    const url =
      `${API_BASE}/commentThreads?part=snippet,replies&maxResults=100` +
      `&order=${order}&videoId=${opts.videoId}` +
      (pageToken ? `&pageToken=${pageToken}` : "") +
      `&key=${opts.apiKey}`;
    const res = await doFetch(url);
    if (!res.ok) {
      const body = await res.text().catch(() => "");
      if (res.status === 403 && isCommentsDisabled(body)) return out; // コメント無効 → skip
      throw new Error(`youtube commentThreads ${res.status}: ${body.slice(0, 200)}`);
    }
    opts.quota?.spend(YT_COST.list, "commentThreads.list");
    const data = (await res.json()) as { items?: CommentThreadRaw[]; nextPageToken?: string };

    for (const thread of data.items ?? []) {
      for (const u of mapCommentThread(thread, opts.videoId, opts.gameSlug)) {
        out.push(u);
        if (opts.maxComments && out.length >= opts.maxComments) break;
      }
      if (opts.maxComments && out.length >= opts.maxComments) break;
    }
    opts.onPage?.(out.length);
    if (opts.maxComments && out.length >= opts.maxComments) break;

    pageToken = data.nextPageToken;
    if (!pageToken) break;
  }
  return opts.maxComments ? out.slice(0, opts.maxComments) : out;
}
