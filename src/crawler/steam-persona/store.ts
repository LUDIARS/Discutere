/**
 * steam-persona sidecar — spec/feature/crawler/STEAM-PERSONA.md §3.
 *
 * 横断レビュアー検出の状態 (追跡アプリ / アプリ×投稿者の出現 / 検出済み投稿者) を
 * 持つ小さな SQLite。utterances 本体や KG の schema には触れない
 * (ingested-store / raw-store と同じ sidecar 思想)。
 */

import fs from "node:fs";
import path from "node:path";

import Database from "better-sqlite3";

/** 追跡対象アプリ (新作でレビュー数が閾値を超えたもの)。 */
export interface TrackedApp {
  appId: number;
  title: string;
  totalReviews: number;
  firstSeenAt: number;
  lastCheckedAt: number | null;
  /** 前回取得時の最新 recommendationid (増分取得の打ち切り位置)。 */
  lastRecommendationId: string | null;
}

/** 複数の追跡アプリにレビューを書いていた投稿者。 */
export interface CrossAuthor {
  steamId: string;
  appCount: number;
  detectedAt: number;
  /** 集中収集が完了した時刻 (未収集は null)。 */
  collectedAt: number | null;
  collectedReviews: number | null;
}

export interface SteamPersonaStore {
  upsertApp(input: { appId: number; title: string; totalReviews: number; at: number }): void;
  listApps(): TrackedApp[];
  markAppChecked(appId: number, at: number, lastRecommendationId?: string): void;
  /** アプリ×投稿者の出現を記録する (冪等)。 */
  addAuthorSighting(input: { appId: number; steamId: string; recommendationId: string; at: number }): void;
  /** minApps 本以上の追跡アプリに出現し、未検出の投稿者を検出登録して返す。 */
  detectCrossAuthors(minApps: number, at: number): CrossAuthor[];
  /** 集中収集が未完了の検出済み投稿者。 */
  listUncollectedAuthors(): CrossAuthor[];
  markAuthorCollected(steamId: string, at: number, reviewCount: number): void;
  close(): void;
}

export const DEFAULT_STEAM_PERSONA_DB_PATH = path.resolve("./data/external/.steam-persona.sqlite");

export function openSteamPersonaStore(
  dbPath: string = DEFAULT_STEAM_PERSONA_DB_PATH
): SteamPersonaStore {
  fs.mkdirSync(path.dirname(dbPath), { recursive: true });
  const db = new Database(dbPath);
  db.pragma("journal_mode = WAL");
  db.exec(`
    CREATE TABLE IF NOT EXISTS tracked_app (
      app_id INTEGER PRIMARY KEY,
      title TEXT NOT NULL,
      total_reviews INTEGER NOT NULL,
      first_seen_at INTEGER NOT NULL,
      last_checked_at INTEGER,
      last_recommendation_id TEXT
    );
    CREATE TABLE IF NOT EXISTS author_sighting (
      app_id INTEGER NOT NULL,
      steam_id TEXT NOT NULL,
      recommendation_id TEXT NOT NULL,
      seen_at INTEGER NOT NULL,
      PRIMARY KEY (app_id, steam_id)
    );
    CREATE INDEX IF NOT EXISTS idx_sighting_steam_id ON author_sighting (steam_id);
    CREATE TABLE IF NOT EXISTS cross_author (
      steam_id TEXT PRIMARY KEY,
      app_count INTEGER NOT NULL,
      detected_at INTEGER NOT NULL,
      collected_at INTEGER,
      collected_reviews INTEGER
    );
  `);

  const upsertAppStmt = db.prepare(`
    INSERT INTO tracked_app (app_id, title, total_reviews, first_seen_at)
      VALUES (@appId, @title, @totalReviews, @at)
    ON CONFLICT(app_id) DO UPDATE SET total_reviews = excluded.total_reviews
  `);
  const listAppsStmt = db.prepare(
    "SELECT app_id, title, total_reviews, first_seen_at, last_checked_at, last_recommendation_id FROM tracked_app ORDER BY app_id"
  );
  const markCheckedStmt = db.prepare(
    "UPDATE tracked_app SET last_checked_at = ?, last_recommendation_id = COALESCE(?, last_recommendation_id) WHERE app_id = ?"
  );
  const addSightingStmt = db.prepare(`
    INSERT OR IGNORE INTO author_sighting (app_id, steam_id, recommendation_id, seen_at)
      VALUES (@appId, @steamId, @recommendationId, @at)
  `);
  const detectStmt = db.prepare(`
    SELECT steam_id, COUNT(DISTINCT app_id) AS app_count FROM author_sighting
      WHERE steam_id NOT IN (SELECT steam_id FROM cross_author)
      GROUP BY steam_id HAVING app_count >= ?
  `);
  const insertCrossStmt = db.prepare(
    "INSERT INTO cross_author (steam_id, app_count, detected_at) VALUES (?, ?, ?)"
  );
  const uncollectedStmt = db.prepare(
    "SELECT steam_id, app_count, detected_at, collected_at, collected_reviews FROM cross_author WHERE collected_at IS NULL ORDER BY app_count DESC, steam_id"
  );
  const markCollectedStmt = db.prepare(
    "UPDATE cross_author SET collected_at = ?, collected_reviews = ? WHERE steam_id = ?"
  );

  const toApp = (r: Record<string, unknown>): TrackedApp => ({
    appId: r.app_id as number,
    title: r.title as string,
    totalReviews: r.total_reviews as number,
    firstSeenAt: r.first_seen_at as number,
    lastCheckedAt: (r.last_checked_at as number | null) ?? null,
    lastRecommendationId: (r.last_recommendation_id as string | null) ?? null,
  });
  const toAuthor = (r: Record<string, unknown>): CrossAuthor => ({
    steamId: r.steam_id as string,
    appCount: r.app_count as number,
    detectedAt: r.detected_at as number,
    collectedAt: (r.collected_at as number | null) ?? null,
    collectedReviews: (r.collected_reviews as number | null) ?? null,
  });

  return {
    upsertApp: (input) => {
      upsertAppStmt.run(input);
    },
    listApps: () => (listAppsStmt.all() as Record<string, unknown>[]).map(toApp),
    markAppChecked: (appId, at, lastRecommendationId) => {
      markCheckedStmt.run(at, lastRecommendationId ?? null, appId);
    },
    addAuthorSighting: (input) => {
      addSightingStmt.run(input);
    },
    detectCrossAuthors: (minApps, at) => {
      const rows = detectStmt.all(minApps) as Record<string, unknown>[];
      const detected: CrossAuthor[] = [];
      for (const r of rows) {
        const steamId = r.steam_id as string;
        const appCount = r.app_count as number;
        insertCrossStmt.run(steamId, appCount, at);
        detected.push({ steamId, appCount, detectedAt: at, collectedAt: null, collectedReviews: null });
      }
      return detected;
    },
    listUncollectedAuthors: () =>
      (uncollectedStmt.all() as Record<string, unknown>[]).map(toAuthor),
    markAuthorCollected: (steamId, at, reviewCount) => {
      markCollectedStmt.run(at, reviewCount, steamId);
    },
    close: () => db.close(),
  };
}
