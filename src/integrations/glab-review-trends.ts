import type Database from "better-sqlite3";

import { getConfig } from "../config.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { listExcludedIds } from "../core/noise/exclusions.js";
import {
  openAttributionStore,
  type AttributionStore,
} from "../crawler/sources/attribution-store.js";

const REVIEW_WINDOW_MS = 7 * 24 * 60 * 60 * 1_000;
const MAX_SOURCE_RECORDS = 100_000;

export interface StoredReviewSignal {
  gameSlug: string | null;
  appId: number | null;
  postedAt: number;
  reaction: "positive" | "negative" | null;
}

export interface GameReviewTrend {
  gameSlug: string;
  appId: number | null;
  recentReviewCount: number;
  positivePercent: number | null;
  latestReviewAt: string;
}

/** attribution と active KG を結合し、集計に必要な匿名化済み信号だけを取り出す。 */
export function readStoredSteamReviewSignals(
  db: Database.Database,
  attribution: Pick<AttributionStore, "listBySource">,
): StoredReviewSignal[] {
  const excluded = listExcludedIds(db);
  const utterance = db.prepare("SELECT posted_at FROM utterances WHERE id = ?");
  // importer が Steam の voted_up を保存する決定的 ID の reaction だけを採用する。
  // 後から同じ発話へ付いた positive/negative reaction でレビュー評価を上書きしない。
  const reaction = db.prepare(
    "SELECT reaction_type FROM reactions WHERE id = ? AND utterance_id = ?",
  );
  const records: StoredReviewSignal[] = [];
  for (const source of attribution.listBySource("steam", null, MAX_SOURCE_RECORDS)) {
    if (excluded.has(source.utteranceId)) continue;
    const utteranceRow = utterance.get(source.utteranceId) as { posted_at: number } | undefined;
    if (!utteranceRow) continue;
    const reactionRow = reaction.get(
      `${source.utteranceId}:vote`,
      source.utteranceId,
    ) as { reaction_type: string } | undefined;
    const reactionType = reactionRow?.reaction_type;
    records.push({
      gameSlug: source.gameSlug,
      appId: steamAppIdFromSourceUrl(source.sourceUrl),
      postedAt: utteranceRow.posted_at,
      reaction: reactionType === "positive" || reactionType === "negative" ? reactionType : null,
    });
  }
  return records;
}

/** Di に取り込み済みのレビュー記録だけを、直近のゲーム別傾向へ縮約する。 */
export function analyzeStoredReviewTrends(
  records: readonly StoredReviewSignal[],
  now = Date.now(),
  limit = 8,
): GameReviewTrend[] {
  const cutoff = now - REVIEW_WINDOW_MS;
  const groups = new Map<string, {
    count: number;
    rated: number;
    positive: number;
    latest: number;
    appIds: Set<number>;
  }>();

  for (const record of records) {
    const gameSlug = record.gameSlug?.trim();
    if (!gameSlug || !Number.isFinite(record.postedAt) || record.postedAt < cutoff || record.postedAt > now) {
      continue;
    }
    const group = groups.get(gameSlug) ?? {
      count: 0,
      rated: 0,
      positive: 0,
      latest: 0,
      appIds: new Set<number>(),
    };
    group.count += 1;
    group.latest = Math.max(group.latest, record.postedAt);
    if (record.appId !== null) group.appIds.add(record.appId);
    if (record.reaction) {
      group.rated += 1;
      if (record.reaction === "positive") group.positive += 1;
    }
    groups.set(gameSlug, group);
  }

  return [...groups.entries()]
    .map(([gameSlug, group]) => ({
      gameSlug,
      appId: group.appIds.size === 1 ? [...group.appIds][0] : null,
      recentReviewCount: group.count,
      positivePercent: group.rated > 0 ? Math.round((group.positive / group.rated) * 100) : null,
      latestReviewAt: new Date(group.latest).toISOString(),
    }))
    .sort((left, right) => (
      right.recentReviewCount - left.recentReviewCount
      || Date.parse(right.latestReviewAt) - Date.parse(left.latestReviewAt)
      || left.gameSlug.localeCompare(right.gameSlug)
    ))
    .slice(0, Math.max(0, limit));
}

/** active KG の Steam 取得記録を読み、本文・投稿者を外へ出さない集計だけを返す。 */
export function loadStoredSteamReviewTrends(now = Date.now(), limit = 8): GameReviewTrend[] {
  const core = createCore(resolveActiveKgPath(getConfig()));
  try {
    const attribution = openAttributionStore();
    try {
      return analyzeStoredReviewTrends(
        readStoredSteamReviewSignals(core.client.raw, attribution),
        now,
        limit,
      );
    } finally {
      attribution.close();
    }
  } finally {
    core.close();
  }
}

/** Steam collector が attribution に残した URL から取得対象の App ID を復元する。 */
export function steamAppIdFromSourceUrl(value: string): number | null {
  try {
    const url = new URL(value);
    const host = url.hostname.toLowerCase();
    if (url.protocol !== "https:" || (host !== "steamcommunity.com" && host !== "store.steampowered.com")) {
      return null;
    }
    const match = /\/(?:recommended|app)\/(\d+)(?:\/|$)/.exec(url.pathname);
    const appId = match ? Number(match[1]) : NaN;
    return Number.isSafeInteger(appId) && appId > 0 ? appId : null;
  } catch {
    return null;
  }
}
