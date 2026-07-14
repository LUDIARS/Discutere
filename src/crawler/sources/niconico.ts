/**
 * niconico (ニコニコ動画) collector (Phase 1) — spec の type union "niconico"。
 *
 * Snapshot Search API v2 で人気動画を発見 → 各動画の watch ページから nvComment 設定を抽出
 * → nvComment スレッド API でコメント (弾幕) を取得し ExternalUtterance に正規化する。
 * API キー不要。日本語の生反応 (賛否が割れやすい) を取り込む。
 *
 * map 系は純粋関数 (テスト用)、fetch 系がネットワーク I/O。
 */

import { setTimeout as sleep } from "node:timers/promises";

import type { ExternalUtterance } from "./types.js";

const SOURCE = "niconico" as const;
const USER_AGENT = "LUDIARS-Discutere-Crawler/0.1 (+https://github.com/LUDIARS)";

export interface NicoVideoRef {
  contentId: string;
  title: string;
  viewCounter?: number;
  commentCounter?: number;
}

/** Snapshot Search API v2 で動画を viewCount 順に発見する。 */
export async function searchNicoVideos(
  query: string,
  limit: number,
  doFetch: typeof fetch = fetch
): Promise<NicoVideoRef[]> {
  const url =
    `https://snapshot.search.nicovideo.jp/api/v2/snapshot/video/contents/search?` +
    `q=${encodeURIComponent(query)}&targets=title,description,tags` +
    `&fields=contentId,title,viewCounter,commentCounter&_sort=-viewCounter&_limit=${limit}`;
  const res = await doFetch(url, { headers: { "User-Agent": USER_AGENT } });
  if (!res.ok) throw new Error(`niconico search HTTP ${res.status}`);
  const data = (await res.json()) as { data?: NicoVideoRef[] };
  return data.data ?? [];
}

export interface NvComment {
  threadKey: string;
  server: string;
  params: { targets: Array<{ id: string; fork: string }>; language?: string };
}

/** watch ページ HTML の <meta name="server-response"> から nvComment 設定を取り出す (純粋関数)。 */
export function extractNvComment(html: string): NvComment | null {
  const m = html.match(/<meta name="server-response" content="([^"]+)"/);
  if (!m) return null;
  const json = m[1]
    .replace(/&quot;/g, '"')
    .replace(/&amp;/g, "&")
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/&#39;/g, "'");
  try {
    const sr = JSON.parse(json) as {
      data?: { response?: { comment?: { nvComment?: NvComment } } };
    };
    return sr.data?.response?.comment?.nvComment ?? null;
  } catch {
    return null;
  }
}

export interface NicoComment {
  id?: string;
  no?: number;
  body: string;
  postedAt?: string;
  userId?: string;
  vposMs?: number;
}

/** 1 動画の watch → nvComment → コメント取得。 */
export async function fetchNicoComments(
  contentId: string,
  doFetch: typeof fetch = fetch
): Promise<NicoComment[]> {
  const watch = await doFetch(`https://www.nicovideo.jp/watch/${contentId}`, {
    headers: { "User-Agent": USER_AGENT, "Accept-Language": "ja" },
  });
  if (!watch.ok) throw new Error(`niconico watch HTTP ${watch.status} (${contentId})`);
  const html = await watch.text();
  const nv = extractNvComment(html);
  if (!nv) return [];

  const res = await doFetch(`${nv.server}/v1/threads`, {
    method: "POST",
    headers: {
      "User-Agent": USER_AGENT,
      "Content-Type": "application/json",
      "x-frontend-id": "6",
      "x-frontend-version": "0",
    },
    body: JSON.stringify({ threadKey: nv.threadKey, params: nv.params, additionals: {} }),
  });
  if (!res.ok) throw new Error(`nvComment HTTP ${res.status} (${contentId})`);
  const data = (await res.json()) as {
    data?: { threads?: Array<{ fork?: string; comments?: NicoComment[] }> };
  };
  const threads = data.data?.threads ?? [];
  const main = threads.find((t) => t.fork === "main") ?? threads[0];
  return main?.comments ?? threads.flatMap((t) => t.comments ?? []);
}

/** niconico コメント 1 件 → ExternalUtterance (純粋関数)。 */
export function mapNicoComment(
  contentId: string,
  gameSlug: string,
  c: NicoComment
): ExternalUtterance {
  const key = c.id ?? String(c.no ?? "");
  return {
    source: SOURCE,
    nativeId: `${contentId}:${key}`,
    gameSlug,
    threadKey: contentId, // 動画 = 議論の場 (session)
    content: c.body ?? "",
    lang: "ja",
    postedAt: c.postedAt ? Date.parse(c.postedAt) : 0,
    videoOffsetMs: c.vposMs,
    // userId は匿名化済みハッシュで公開・安定。取れない弾幕は動画内 no で擬似アンカー化
    authorId: c.userId ? `nico:${c.userId}` : `nico-anon:${contentId}:${key}`,
    sourceUrl: `https://www.nicovideo.jp/watch/${contentId}`,
  };
}

export interface NicoFetchOptions {
  gameSlug: string;
  query: string;
  /** 取得する動画数 (viewCount 上位)。既定 5 */
  maxVideos?: number;
  /** 動画あたりのコメント上限。既定 無制限 */
  maxComments?: number;
  politeDelayMs?: number;
  fetchImpl?: typeof fetch;
  onVideo?: (info: { videos: number; comments: number }) => void;
}

/** 動画発見 → コメント収集を一括で行う。 */
export async function fetchNiconicoDiscussions(
  opts: NicoFetchOptions
): Promise<ExternalUtterance[]> {
  const doFetch = opts.fetchImpl ?? fetch;
  const delay = opts.politeDelayMs ?? 1500;
  const videos = await searchNicoVideos(opts.query, opts.maxVideos ?? 5, doFetch);

  const out: ExternalUtterance[] = [];
  let vi = 0;
  for (const v of videos) {
    await sleep(delay);
    try {
      const comments = await fetchNicoComments(v.contentId, doFetch);
      const slice = opts.maxComments ? comments.slice(0, opts.maxComments) : comments;
      for (const c of slice) {
        const u = mapNicoComment(v.contentId, opts.gameSlug, c);
        if (u.content.trim().length > 0) out.push(u);
      }
    } catch {
      // 1 動画の失敗は致命にしない (非公開/削除/コメント不可)
    }
    vi += 1;
    opts.onVideo?.({ videos: vi, comments: out.length });
  }
  return out;
}
