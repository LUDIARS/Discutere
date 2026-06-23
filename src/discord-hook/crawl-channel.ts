/**
 * データクロール用チャンネル — 貼られたリンクから外部議論データを学習する (id=60)。
 *
 * 専用チャンネル (config `discord.crawlChannelIds`) に URL が貼られたら、 URL の種類を
 * 判定 (steam / youtube / reddit / website) して該当 collector を回し、 Discatier Core に取り込む。
 * 取り込み結果は「データ追加」 通知チャンネル (system-channel.ts) に投稿する。
 *
 * URL 判定・slug 導出・メッセージ解析は純粋関数 (テスト可能)、 `ingestCrawlUrls` が I/O。
 */

import type { createCore } from "../core/index.js";
import { fetchSteamReviews } from "../crawler/sources/steam.js";
import { fetchVideoComments } from "../crawler/sources/youtube-comments.js";
import { createQuotaTracker } from "../crawler/sources/youtube-quota.js";
import { fetchWebsiteArticles } from "../crawler/sources/website.js";
import { getRedditToken, fetchThreadComments, type RedditCredentials } from "../crawler/sources/reddit.js";
import { importExternalUtterances } from "../crawler/sources/importer.js";
import { openAttributionStore } from "../crawler/sources/attribution-store.js";
import { openIngestedStore } from "../crawler/sources/ingested-store.js";
import { openRawStore } from "../crawler/sources/raw-store.js";
import { summarizeItems, type Summarizer } from "../crawler/sources/summarize.js";

type Core = ReturnType<typeof createCore>;

export type CrawlKind = "steam" | "youtube" | "reddit" | "website";

export interface CrawlTarget {
  kind: CrawlKind;
  url: string;
  /** steam appId (kind=steam) */
  appId?: number;
  /** youtube videoId (kind=youtube) */
  videoId?: string;
  /** reddit submission id + permalink (kind=reddit) */
  submissionId?: string;
  permalink?: string;
}

/** 1 URL あたりの取得上限 (1 回の貼り付けで KG を埋め尽くさないための安全弁)。 */
export const CRAWL_CAPS = { steamReviews: 500, youtubeComments: 500, redditComments: 500, websiteArticles: 1 } as const;

const URL_RE = /https?:\/\/[^\s<>"'）)】」]+/gi;

/** メッセージ本文から URL を抽出 (末尾の句読点を落とす)。 */
export function extractUrls(text: string): string[] {
  const found = text.match(URL_RE) ?? [];
  return found.map((u) => u.replace(/[.,。、)）】」]+$/, "")).filter((u) => u.length > 0);
}

/**
 * crawl メッセージを解析。 先頭に `[slug]` / `slug:` 形式があればゲーム slug ヒントとして拾い、
 * 残りから URL を抽出する。 slug が無ければ undefined (URL から導出する)。
 */
export function parseCrawlMessage(content: string): { gameSlug?: string; urls: string[] } {
  const trimmed = content.trim();
  let gameSlug: string | undefined;
  let rest = trimmed;
  const bracket = trimmed.match(/^\[([a-z0-9][a-z0-9-]{0,63})\]\s*/i);
  const colon = trimmed.match(/^([a-z0-9][a-z0-9-]{0,63}):\s+(?=https?:\/\/)/i);
  if (bracket) {
    gameSlug = bracket[1].toLowerCase();
    rest = trimmed.slice(bracket[0].length);
  } else if (colon) {
    gameSlug = colon[1].toLowerCase();
    rest = trimmed.slice(colon[0].length);
  }
  return { gameSlug, urls: extractUrls(rest) };
}

/** URL → 取得元種別 + パラメータ。 */
export function classifyCrawlUrl(rawUrl: string): CrawlTarget {
  let u: URL;
  try {
    u = new URL(rawUrl);
  } catch {
    return { kind: "website", url: rawUrl };
  }
  const host = u.hostname.replace(/^www\./, "");

  // Steam: store.steampowered.com/app/<id>/... or steamcommunity.com/app/<id>
  if (host === "store.steampowered.com" || host === "steamcommunity.com") {
    const m = u.pathname.match(/\/app\/(\d+)/);
    if (m) return { kind: "steam", url: rawUrl, appId: Number(m[1]) };
  }

  // YouTube watch: youtube.com/watch?v=<id> / youtu.be/<id> / youtube.com/shorts/<id>
  if (host === "youtube.com" || host === "m.youtube.com" || host === "youtu.be") {
    let videoId: string | null = null;
    if (host === "youtu.be") videoId = u.pathname.slice(1) || null;
    else if (u.pathname === "/watch") videoId = u.searchParams.get("v");
    else {
      const s = u.pathname.match(/\/(?:shorts|live|embed)\/([\w-]{6,})/);
      if (s) videoId = s[1];
    }
    if (videoId) return { kind: "youtube", url: rawUrl, videoId };
  }

  // Reddit: reddit.com/r/<sub>/comments/<id>/... or /comments/<id>
  if (host === "reddit.com" || host === "old.reddit.com" || host === "np.reddit.com") {
    const m = u.pathname.match(/\/comments\/([a-z0-9]+)/i);
    if (m) return { kind: "reddit", url: rawUrl, submissionId: m[1], permalink: u.pathname };
  }

  return { kind: "website", url: rawUrl };
}

function slugify(value: string): string {
  return (
    value
      .normalize("NFKD")
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, "-")
      .replace(/^-+|-+$/g, "")
      .slice(0, 48) || "web"
  );
}

/** slug ヒントが無いとき、 URL 種別から fallback slug を導出。 */
export function slugFromTarget(t: CrawlTarget): string {
  if (t.kind === "steam" && t.appId) return `steam-${t.appId}`;
  if (t.kind === "youtube" && t.videoId) return `yt-${t.videoId}`;
  if (t.kind === "reddit" && t.submissionId) return `reddit-${t.submissionId}`;
  try {
    return slugify(new URL(t.url).hostname.replace(/^www\./, ""));
  } catch {
    return "web";
  }
}

export interface CrawlIngestResult {
  url: string;
  kind: CrawlKind;
  gameSlug: string;
  fetched: number;
  imported: number;
  skipped: number;
  error?: string;
}

export interface CrawlDeps {
  createCore: () => Core;
  workspaceId: string;
  /** YouTube Data API key (未設定なら youtube URL はエラーにする) */
  youtubeApiKey?: string | null;
  getYoutubeApiKey?: () => Promise<string | null>;
  /** Reddit OAuth 認証 (未設定なら reddit URL はエラーにする) */
  reddit?: RedditCredentials | null;
  /** website 記事の要約器 (id=67)。 渡せば長文 website を要約/raw 2 層で取り込む。 */
  summarizer?: Summarizer | null;
}

async function fetchForTarget(t: CrawlTarget, gameSlug: string, deps: CrawlDeps) {
  if (t.kind === "steam" && t.appId) {
    return fetchSteamReviews({ appId: t.appId, gameSlug, maxReviews: CRAWL_CAPS.steamReviews });
  }
  if (t.kind === "youtube" && t.videoId) {
    const youtubeApiKey = deps.getYoutubeApiKey ? await deps.getYoutubeApiKey() : deps.youtubeApiKey;
    if (!youtubeApiKey) throw new Error("YouTube API key 未設定 (DISCUTERE_YOUTUBE_API_KEY)");
    return fetchVideoComments({
      videoId: t.videoId,
      gameSlug,
      apiKey: youtubeApiKey,
      maxComments: CRAWL_CAPS.youtubeComments,
      quota: createQuotaTracker(),
    });
  }
  if (t.kind === "reddit" && t.submissionId) {
    if (!deps.reddit) throw new Error("Reddit 認証未設定 (DISCUTERE_REDDIT_CLIENT_ID/SECRET)");
    const token = await getRedditToken(deps.reddit);
    return fetchThreadComments({
      token,
      userAgent: deps.reddit.userAgent,
      submissionId: t.submissionId,
      gameSlug,
      permalink: t.permalink,
      maxComments: CRAWL_CAPS.redditComments,
    });
  }
  return fetchWebsiteArticles({ urls: [t.url], gameSlug });
}

/** URL 群を取得 → Discatier Core に取り込み、 結果サマリを返す (URL ごとに独立、 失敗は記録)。 */
export async function ingestCrawlUrls(
  deps: CrawlDeps,
  input: { urls: string[]; gameSlugHint?: string }
): Promise<CrawlIngestResult[]> {
  const out: CrawlIngestResult[] = [];
  for (const url of input.urls) {
    const target = classifyCrawlUrl(url);
    const gameSlug = input.gameSlugHint ?? slugFromTarget(target);
    const base: CrawlIngestResult = { url, kind: target.kind, gameSlug, fetched: 0, imported: 0, skipped: 0 };
    const core = deps.createCore();
    const ingested = openIngestedStore();
    const attribution = openAttributionStore();
    // website (長文) は要約器があれば要約/raw 2 層で取り込む (id=67)。
    const useRawStore = target.kind === "website" && !!deps.summarizer;
    const rawStore = useRawStore ? openRawStore() : undefined;
    try {
      const items = await fetchForTarget(target, gameSlug, deps);
      base.fetched = items.length;
      if (target.kind === "website" && deps.summarizer) {
        await summarizeItems(items, deps.summarizer);
      }
      const r = importExternalUtterances(core, items, {
        workspaceId: deps.workspaceId,
        ingested,
        attribution,
        rawStore,
      });
      base.imported = r.utterances;
      base.skipped = r.skipped;
    } catch (err) {
      base.error = (err as Error).message;
    } finally {
      rawStore?.close();
      attribution.close();
      ingested.close();
      core.close();
    }
    out.push(base);
  }
  return out;
}

/** 結果サマリを 1 行/件の人間向けテキストに整形 (Discord 投稿用)。 */
export function formatCrawlSummary(results: CrawlIngestResult[]): string {
  const icon = (r: CrawlIngestResult) => (r.error ? "⚠️" : r.imported > 0 ? "✅" : "➖");
  const lines = results.map((r) => {
    if (r.error) return `${icon(r)} [${r.kind}] ${r.url}\n   └ 失敗: ${r.error}`;
    return `${icon(r)} [${r.kind}] ${r.gameSlug} ← ${r.imported}件取込 (skip ${r.skipped}) / ${r.url}`;
  });
  const total = results.reduce((s, r) => s + r.imported, 0);
  return `📥 **データ追加** (計 ${total} 件)\n${lines.join("\n")}`;
}
