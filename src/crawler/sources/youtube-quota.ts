/**
 * YouTube Data API quota トラッカー (Phase 1) — spec/feature/crawler/EXTERNAL-SOURCES.md §4.2。
 *
 * 無料枠 = 10,000 units/日 をプロセス内で加算管理する。上限超過で canSpend が false。
 * search.list は 100 units、 list 系 (commentThreads/playlistItems/videos/channels) は 1 unit。
 */

export const YT_COST = {
  search: 100,
  list: 1,
} as const;

export interface QuotaTracker {
  /** units を消費。残量を超える場合は例外 */
  spend(units: number, label?: string): void;
  /** 消費せず可否だけ判定 */
  canSpend(units: number): boolean;
  spent(): number;
  remaining(): number;
}

export function createQuotaTracker(dailyQuota = 10_000): QuotaTracker {
  let used = 0;
  return {
    spend(units, label) {
      if (used + units > dailyQuota) {
        throw new Error(
          `youtube quota exceeded: need ${units}${label ? ` (${label})` : ""}, used ${used}/${dailyQuota}`
        );
      }
      used += units;
    },
    canSpend: (units) => used + units <= dailyQuota,
    spent: () => used,
    remaining: () => Math.max(0, dailyQuota - used),
  };
}
