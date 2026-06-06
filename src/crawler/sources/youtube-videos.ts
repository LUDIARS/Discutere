/**
 * YouTube 動画クローラ (discovery) — spec/crawler/EXTERNAL-SOURCES.md §4.2(a)。
 *
 * ゲーム名 → コメントを取りに行く候補 動画 id リスト (VideoRef) を作る。
 * quota 節約のため、 チャンネルが分かるなら uploads playlist (1 unit/page) を優先し、
 * キーワード検索 (search.list, 100 units/call) は呼び出し回数を上限化する。
 *
 * `mapSearchVideo` / `mapPlaylistVideo` は純粋関数 (テスト用)。
 */

import { YT_COST, type QuotaTracker } from "./youtube-quota.js";

const API_BASE = "https://www.googleapis.com/youtube/v3";

export interface VideoRef {
  videoId: string;
  title: string;
  channelId?: string;
  publishedAt?: number; // epoch ms
}

// --- pure mappers ---

export interface SearchItemRaw {
  id?: { videoId?: string };
  snippet?: { title?: string; channelId?: string; publishedAt?: string };
}

export function mapSearchVideo(item: SearchItemRaw): VideoRef | null {
  const videoId = item.id?.videoId;
  if (!videoId) return null; // channel/playlist 結果は除外
  return {
    videoId,
    title: item.snippet?.title ?? "",
    channelId: item.snippet?.channelId,
    publishedAt: item.snippet?.publishedAt ? Date.parse(item.snippet.publishedAt) : undefined,
  };
}

export interface PlaylistItemRaw {
  snippet?: { title?: string; videoOwnerChannelId?: string; resourceId?: { videoId?: string } };
  contentDetails?: { videoId?: string; videoPublishedAt?: string };
}

export function mapPlaylistVideo(item: PlaylistItemRaw): VideoRef | null {
  const videoId = item.contentDetails?.videoId ?? item.snippet?.resourceId?.videoId;
  if (!videoId) return null;
  const published = item.contentDetails?.videoPublishedAt;
  return {
    videoId,
    title: item.snippet?.title ?? "",
    channelId: item.snippet?.videoOwnerChannelId,
    publishedAt: published ? Date.parse(published) : undefined,
  };
}

// --- network ---

interface FetchDeps {
  apiKey: string;
  fetchImpl?: typeof fetch;
  quota?: QuotaTracker;
}

async function getJson(url: string, fetchImpl: typeof fetch): Promise<any> {
  const res = await fetchImpl(url);
  if (!res.ok) {
    const body = await res.text().catch(() => "");
    throw new Error(`youtube ${res.status}: ${body.slice(0, 200)}`);
  }
  return res.json();
}

export interface SearchDiscoverOptions extends FetchDeps {
  query: string;
  /** 取得する動画数の上限 (1 call=50件)。既定 50 */
  maxResults?: number;
  /** relevance | date | viewCount。既定 relevance */
  order?: string;
}

/** キーワード検索で動画を探す (search.list, 100 units/call)。 */
export async function discoverVideosBySearch(opts: SearchDiscoverOptions): Promise<VideoRef[]> {
  const doFetch = opts.fetchImpl ?? fetch;
  const max = opts.maxResults ?? 50;
  const order = opts.order ?? "relevance";
  const out: VideoRef[] = [];
  let pageToken: string | undefined;

  while (out.length < max) {
    if (opts.quota && !opts.quota.canSpend(YT_COST.search)) break;
    opts.quota?.spend(YT_COST.search, "search.list");
    const per = Math.min(50, max - out.length);
    const url =
      `${API_BASE}/search?part=snippet&type=video&maxResults=${per}` +
      `&order=${order}&q=${encodeURIComponent(opts.query)}` +
      (pageToken ? `&pageToken=${pageToken}` : "") +
      `&key=${opts.apiKey}`;
    const data = await getJson(url, doFetch);
    for (const item of (data.items ?? []) as SearchItemRaw[]) {
      const ref = mapSearchVideo(item);
      if (ref) out.push(ref);
    }
    pageToken = data.nextPageToken;
    if (!pageToken) break;
  }
  return out.slice(0, max);
}

export interface ChannelDiscoverOptions extends FetchDeps {
  channelId: string;
  maxResults?: number;
}

/** チャンネルの uploads playlist から動画を列挙する (channels.list 1u + playlistItems 1u/page)。 */
export async function discoverVideosByChannel(opts: ChannelDiscoverOptions): Promise<VideoRef[]> {
  const doFetch = opts.fetchImpl ?? fetch;
  const max = opts.maxResults ?? 200;

  opts.quota?.spend(YT_COST.list, "channels.list");
  const chUrl = `${API_BASE}/channels?part=contentDetails&id=${opts.channelId}&key=${opts.apiKey}`;
  const chData = await getJson(chUrl, doFetch);
  const uploads: string | undefined =
    chData.items?.[0]?.contentDetails?.relatedPlaylists?.uploads;
  if (!uploads) return [];

  const out: VideoRef[] = [];
  let pageToken: string | undefined;
  while (out.length < max) {
    if (opts.quota && !opts.quota.canSpend(YT_COST.list)) break;
    opts.quota?.spend(YT_COST.list, "playlistItems.list");
    const per = Math.min(50, max - out.length);
    const url =
      `${API_BASE}/playlistItems?part=snippet,contentDetails&maxResults=${per}` +
      `&playlistId=${uploads}` +
      (pageToken ? `&pageToken=${pageToken}` : "") +
      `&key=${opts.apiKey}`;
    const data = await getJson(url, doFetch);
    for (const item of (data.items ?? []) as PlaylistItemRaw[]) {
      const ref = mapPlaylistVideo(item);
      if (ref) out.push(ref);
    }
    pageToken = data.nextPageToken;
    if (!pageToken) break;
  }
  return out.slice(0, max);
}
