/**
 * Steam レビュー collector (Phase 1) — spec/feature/crawler/EXTERNAL-SOURCES.md §4.1.
 *
 * 公開エンドポイント `store.steampowered.com/appreviews/{appid}?json=1` を
 * cursor ページングで全件取得し、ExternalUtterance に正規化する。API キー不要。
 *
 * `mapSteamReview` は純粋関数 (テスト用)、`fetchSteamReviews` がネットワーク I/O。
 */

import { setTimeout as sleep } from "node:timers/promises";

import type { ExternalUtterance } from "./types.js";

const SOURCE = "steam" as const;
const USER_AGENT = "LUDIARS-Discutere-Crawler/0.1 (+https://github.com/LUDIARS)";

export interface SteamReviewAuthor {
  steamid?: string;
  num_games_owned?: number;
  playtime_forever?: number;
}

export interface SteamReviewRaw {
  recommendationid: string;
  author?: SteamReviewAuthor;
  review: string;
  language?: string;
  timestamp_created: number; // epoch seconds
  voted_up: boolean;
  votes_up?: number;
}

interface SteamApiResponse {
  success: number;
  reviews?: SteamReviewRaw[];
  cursor?: string;
  query_summary?: { total_reviews?: number };
}

/** Steam review 1 件 → ExternalUtterance (純粋関数)。 */
export function mapSteamReview(
  appId: number,
  gameSlug: string,
  raw: SteamReviewRaw
): ExternalUtterance {
  const steamId = raw.author?.steamid ?? "unknown";
  return {
    source: SOURCE,
    nativeId: raw.recommendationid,
    gameSlug,
    threadKey: String(appId),
    content: raw.review ?? "",
    lang: raw.language,
    postedAt: raw.timestamp_created * 1000,
    authorId: steamId, // SteamID64 = 公開・安定な persona アンカー (spec §6)
    parentNativeId: undefined, // Steam レビューはスレッド構造を持たない
    signal: { votedUp: raw.voted_up, upvotes: raw.votes_up },
    sourceUrl: `https://steamcommunity.com/profiles/${steamId}/recommended/${appId}/`,
  };
}

export interface SteamFetchOptions {
  appId: number;
  gameSlug: string;
  /** Steam language フィルタ。既定 ["all"]。日本語のみなら ["japanese"] */
  languages?: string[];
  /** "recent"(全件 cursor 走査向き) | "all" | "updated"。既定 "recent" */
  filter?: string;
  /** 取得上限。0/undefined で無制限 (全件) */
  maxReviews?: number;
  /** 同一ドメインへの最小間隔 ms。既定 1200 */
  politeDelayMs?: number;
  /** DI 用。既定 global fetch */
  fetchImpl?: typeof fetch;
  /** 1 ページ取得ごとの進捗コールバック (累計件数) */
  onPage?: (total: number) => void;
}

/** Steam レビューを cursor 全件 (or maxReviews まで) 取得する。 */
export async function fetchSteamReviews(opts: SteamFetchOptions): Promise<ExternalUtterance[]> {
  const doFetch = opts.fetchImpl ?? fetch;
  const languages = opts.languages && opts.languages.length > 0 ? opts.languages : ["all"];
  const langParam = languages.join(",");
  const filter = opts.filter ?? "recent";
  const delay = opts.politeDelayMs ?? 1200;

  const out: ExternalUtterance[] = [];
  const seenCursors = new Set<string>();
  let cursor = "*";

  for (;;) {
    const url =
      `https://store.steampowered.com/appreviews/${opts.appId}?json=1` +
      `&num_per_page=100&filter=${filter}&language=${encodeURIComponent(langParam)}` +
      `&review_type=all&purchase_type=all&cursor=${encodeURIComponent(cursor)}`;

    const res = await doFetch(url, { headers: { "User-Agent": USER_AGENT } });
    if (!res.ok) throw new Error(`steam appreviews HTTP ${res.status}`);
    const data = (await res.json()) as SteamApiResponse;
    if (data.success !== 1) throw new Error(`steam appreviews success=${data.success}`);

    const reviews = data.reviews ?? [];
    if (reviews.length === 0) break;

    for (const r of reviews) {
      out.push(mapSteamReview(opts.appId, opts.gameSlug, r));
      if (opts.maxReviews && out.length >= opts.maxReviews) break;
    }
    opts.onPage?.(out.length);
    if (opts.maxReviews && out.length >= opts.maxReviews) break;

    const next = data.cursor;
    if (!next || seenCursors.has(next)) break; // 末尾 (cursor が進まなくなる) で停止
    seenCursors.add(next);
    cursor = next;
    await sleep(delay);
  }

  return out;
}
