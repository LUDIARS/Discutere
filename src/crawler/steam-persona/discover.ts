/**
 * Step 1 — 新作発見: store 検索の新作からレビュー総数が閾値以上のものを追跡対象に登録する。
 * spec/feature/crawler/STEAM-PERSONA.md §2-1。
 */

import type { SteamFetchPorts } from "./ports.js";
import type { SteamPersonaStore } from "./store.js";

export interface DiscoverOptions {
  /** 新作一覧から見る件数。 */
  maxApps: number;
  /** 追跡登録するレビュー総数の下限 (これを超えるもの)。 */
  minTotalReviews: number;
  now: () => number;
  log?: (msg: string) => void;
}

export interface DiscoverResult {
  scanned: number;
  tracked: number;
}

/** @implements SPEC-STEAM-PERSONA-PIPELINE — 新作をスキャンし、閾値超えを追跡登録する。 */
export async function discoverNewReleases(
  ports: SteamFetchPorts,
  store: SteamPersonaStore,
  opts: DiscoverOptions
): Promise<DiscoverResult> {
  const entries = await ports.fetchNewReleases({ maxApps: opts.maxApps });
  const known = new Set(store.listApps().map((a) => a.appId));
  let tracked = 0;

  for (const entry of entries) {
    // 既追跡アプリの総数更新は track 側の周回で行う。ここでは新規判定のみ (API 呼び出し節約)。
    if (known.has(entry.appId)) continue;
    const total = await ports.fetchTotalReviews(entry.appId);
    if (total <= opts.minTotalReviews) continue;
    store.upsertApp({ appId: entry.appId, title: entry.title, totalReviews: total, at: opts.now() });
    tracked++;
    opts.log?.(`track app ${entry.appId} "${entry.title}" (${total} reviews)`);
  }

  return { scanned: entries.length, tracked };
}
