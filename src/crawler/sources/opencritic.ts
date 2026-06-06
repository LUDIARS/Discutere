/**
 * OpenCritic collector (Phase 1) — 批評家レビューのスコア付きスニペットを取り込む。
 *
 * 公開 API (`api.opencritic.com`) でゲーム検索 → レビュー一覧を取得し、ExternalUtterance に
 * 正規化する。API キー不要 (Referer/Origin ヘッダのみ)。スコア付きの賛否=Gap Detection の燃料。
 *
 * `mapOpenCriticReview` は純粋関数 (テスト用)、fetch 系がネットワーク I/O。
 */

import { setTimeout as sleep } from "node:timers/promises";

import type { ExternalUtterance } from "./types.js";

const SOURCE = "opencritic" as const;
const USER_AGENT = "LUDIARS-Discutere-Crawler/0.1 (+https://github.com/LUDIARS)";
// OpenCritic 公開 API は 2024 以降 RapidAPI ゲートウェイ経由 (無料枠あり) になった。
const RAPIDAPI_HOST = "opencritic-api.p.rapidapi.com";
const API = `https://${RAPIDAPI_HOST}`;

export interface OpenCriticReviewRaw {
  _id?: string;
  snippet?: string;
  score?: number; // 媒体スコア (0-100 等、媒体依存)
  externalUrl?: string;
  publishedDate?: string;
  Outlet?: { name?: string };
  Authors?: Array<{ name?: string }>;
}

/** OpenCritic レビュー 1 件 → ExternalUtterance (純粋関数)。 */
export function mapOpenCriticReview(
  gameId: number,
  gameSlug: string,
  raw: OpenCriticReviewRaw
): ExternalUtterance {
  const outlet = raw.Outlet?.name ?? "unknown";
  const author = raw.Authors?.[0]?.name;
  return {
    source: SOURCE,
    nativeId: raw._id ?? `${outlet}:${raw.publishedDate ?? ""}`,
    gameSlug,
    threadKey: String(gameId), // ゲーム = 議論の場
    content: raw.snippet ?? "",
    lang: "en",
    postedAt: raw.publishedDate ? Date.parse(raw.publishedDate) : 0,
    authorId: `outlet:${outlet}`, // 媒体名 = 公開・安定アンカー (spec §6)
    authorName: author ?? outlet,
    signal: typeof raw.score === "number" ? { upvotes: Math.round(raw.score) } : undefined,
    sourceUrl: raw.externalUrl ?? `https://opencritic.com/game/${gameId}`,
  };
}

async function ocFetch(url: string, apiKey: string, doFetch: typeof fetch): Promise<unknown> {
  const res = await doFetch(url, {
    headers: {
      "User-Agent": USER_AGENT,
      Accept: "application/json",
      "X-RapidAPI-Key": apiKey,
      "X-RapidAPI-Host": RAPIDAPI_HOST,
    },
  });
  if (!res.ok) throw new Error(`opencritic HTTP ${res.status} (${url})`);
  return res.json();
}

/** ゲーム名 → OpenCritic game id (最良一致)。見つからなければ null。 */
export async function searchOpenCriticGame(
  query: string,
  apiKey: string,
  doFetch: typeof fetch = fetch
): Promise<number | null> {
  const res = (await ocFetch(
    `${API}/game/search?criteria=${encodeURIComponent(query)}`,
    apiKey,
    doFetch
  )) as Array<{ id: number; name: string; dist: number }>;
  if (!Array.isArray(res) || res.length === 0) return null;
  return res.slice().sort((a, b) => (a.dist ?? 1) - (b.dist ?? 1))[0].id;
}

export interface OpenCriticFetchOptions {
  gameSlug: string;
  /** ゲーム名 (検索用) */
  query: string;
  /** RapidAPI (opencritic-api) のキー。env DISCUTERE_OPENCRITIC_RAPIDAPI_KEY 由来 */
  apiKey: string;
  /** game id を直接指定する場合 (名前検索を飛ばす) */
  gameId?: number;
  maxReviews?: number;
  politeDelayMs?: number;
  fetchImpl?: typeof fetch;
  onPage?: (total: number) => void;
}

/** OpenCritic の批評家レビューを取得して正規化する。 */
export async function fetchOpenCriticReviews(
  opts: OpenCriticFetchOptions
): Promise<ExternalUtterance[]> {
  const doFetch = opts.fetchImpl ?? fetch;
  const delay = opts.politeDelayMs ?? 1200;

  const gameId = opts.gameId ?? (await searchOpenCriticGame(opts.query, opts.apiKey, doFetch));
  if (!gameId) throw new Error(`opencritic: game not found for "${opts.query}"`);
  await sleep(delay);

  const reviews = (await ocFetch(
    `${API}/review/game/${gameId}?sort=popularity`,
    opts.apiKey,
    doFetch
  )) as OpenCriticReviewRaw[];
  const list = Array.isArray(reviews) ? reviews : [];
  const max = opts.maxReviews && opts.maxReviews > 0 ? opts.maxReviews : list.length;

  const out: ExternalUtterance[] = [];
  for (const r of list.slice(0, max)) {
    const u = mapOpenCriticReview(gameId, opts.gameSlug, r);
    if (u.content.trim().length > 0) out.push(u);
  }
  opts.onPage?.(out.length);
  return out;
}
