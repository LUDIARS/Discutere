/**
 * Step 2 — 定期取得: 追跡アプリのレビューを増分取得し、アプリ×投稿者の出現を記録する。
 * spec/feature/crawler/STEAM-PERSONA.md §2-2。
 *
 * レビュー本文はここでは保存しない (出現の記録が目的)。本文の元データ化は
 * 横断検出後の集中収集 (collect) が担う。
 */

import { isSteamId64, type SteamFetchPorts } from "./ports.js";
import type { SteamPersonaStore } from "./store.js";

export interface TrackOptions {
  /** 1 アプリあたりの増分取得上限。 */
  maxReviewsPerApp: number;
  now: () => number;
  log?: (msg: string) => void;
}

export interface TrackResult {
  apps: number;
  sightings: number;
}

/** @implements SPEC-STEAM-PERSONA-PIPELINE — 新規レビューの (app, SteamID64) 出現を記録する。 */
export async function trackAppReviews(
  ports: SteamFetchPorts,
  store: SteamPersonaStore,
  opts: TrackOptions
): Promise<TrackResult> {
  const apps = store.listApps();
  let sightings = 0;

  for (const app of apps) {
    const reviews = await ports.fetchAppReviews({
      appId: app.appId,
      maxReviews: opts.maxReviewsPerApp,
      stopAtRecommendationId: app.lastRecommendationId ?? undefined,
    });
    const at = opts.now();
    for (const r of reviews) {
      const steamId = r.author?.steamid;
      if (!steamId || !isSteamId64(steamId)) continue; // 匿名・不正 ID は横断同定の対象外
      store.addAuthorSighting({
        appId: app.appId,
        steamId,
        recommendationId: r.recommendationid,
        at,
      });
      sightings++;
    }
    // filter=recent の先頭 = 最新。次回はここで打ち切る。
    store.markAppChecked(app.appId, at, reviews[0]?.recommendationid);
    if (reviews.length > 0) opts.log?.(`app ${app.appId}: ${reviews.length} new reviews`);
  }

  return { apps: apps.length, sightings };
}
