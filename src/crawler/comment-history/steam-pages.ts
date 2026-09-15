import { mapSteamReview, type SteamReviewRaw } from "../sources/steam.js";
import type { HistoryStore } from "./store.js";
import type { HistoryJob, HistoryRecord } from "./types.js";
import { historyStart } from "./window.js";

export async function collectSteamPage(job: HistoryJob, store: HistoryStore, now: number, fetcher: typeof fetch,
  clock: () => number = () => now): Promise<void> {
  const url = new URL(`https://store.steampowered.com/appreviews/${job.target}`);
  url.search = new URLSearchParams({ json: "1", filter: "recent", language: "all", num_per_page: "100",
    review_type: "all", purchase_type: "all", cursor: job.cursor ?? "*" }).toString();
  let response: Response;
  try { response = await fetcher(url, { signal: AbortSignal.timeout(30_000), redirect: "error",
    headers: { "User-Agent": "LUDIARS-Discutere-CommentHistory/1.0" } }); }
  catch { throw new Error("steam_request_failed"); }
  if (!response.ok) throw new Error(`steam_http_${response.status}`);
  const data = await response.json() as { success?: number; reviews?: SteamReviewRaw[]; cursor?: string };
  now = clock();
  if (data.success !== 1 || !Array.isArray(data.reviews)) throw new Error("steam_invalid_response");
  const rows: HistoryRecord[] = [];
  const since = historyStart(now);
  for (const raw of data.reviews) {
    const row = mapSteamReview(Number(job.target), job.topic, raw);
    if (!row.nativeId || !row.authorId || row.authorId === "unknown" || !row.content.trim() ||
      !Number.isFinite(row.postedAt) || row.postedAt < since || row.postedAt > now) continue;
    delete row.authorName;
    rows.push({ ...row, source: "steam", fetchedAt: now, expiresAt: now + 186 * 86400_000 });
  }
  const reachedWindow = data.reviews.some(row => Number.isFinite(row.timestamp_created) && row.timestamp_created * 1000 < since);
  const done = !data.reviews.length || reachedWindow || !data.cursor;
  if (!done && data.cursor === job.cursor) throw new Error("steam_repeated_cursor");
  store.save({ ...job, cursor: done ? undefined : data.cursor, status: done ? "complete" : "pending",
    nextAt: done ? now + 86400_000 : now + 1200, detail: done ? "target_window_observed" : "pagination_incomplete" }, rows);
}
