/**
 * source-attribution sidecar — spec/feature/crawler/EXTERNAL-SOURCES.md §3 / §6.
 *
 * 露出制御で「ソース種別 + 元 URL を end user に開示」するため、 取り込み時に
 * utterance ごとの出所 (source / source_url / native_id / game_slug) を引けるようにする。
 * utterances 本体の schema は不変 (ingested-store / raw-store と同じ思想)。
 *
 * 個人アンカー (authorId / authorName) はここに保持しない (§6 露出マスク対象なので別レイヤー)。
 * データ調整 UX (§13) はこのテーブルを source / game_slug で集計・一覧する。
 */

import fs from "node:fs";
import path from "node:path";

import Database from "better-sqlite3";

import { resolveActiveKgDir } from "../../core/kg-registry.js";
import { getConfig } from "../../config.js";
import type { ExternalSource } from "./types.js";

export interface Attribution {
  utteranceId: string;
  source: string;
  sourceUrl: string;
  nativeId: string;
  gameSlug: string | null;
  importedAt: number;
}

/** source 別の集計 1 行 (データ調整 UX のバッジ・一覧用)。 */
export interface SourceCount {
  source: string;
  count: number;
  gameSlugs: string[];
  lastImportedAt: number | null;
}

export interface AttributionStore {
  /** utteranceId に出所メタを紐付けて保存 (upsert)。 */
  set(input: {
    utteranceId: string;
    source: ExternalSource | string;
    sourceUrl: string;
    nativeId: string;
    gameSlug?: string | null;
    at?: number;
  }): void;
  /** utteranceId の出所メタを引く。 無ければ null (内部 Discord 発話など)。 */
  get(utteranceId: string): Attribution | null;
  /** source (任意で gameSlug) に属する取り込み発話を新しい順に列挙する。 */
  listBySource(source: string, gameSlug?: string | null, limit?: number): Attribution[];
  /** source 別の件数・対象ゲーム・直近取り込み時刻を集計する。 */
  countsBySource(): SourceCount[];
  close(): void;
}

/** active KG ディレクトリ配下の既定パス (§12 — KG 切替で帰属も切り替わる)。 */
export function defaultAttributionPath(): string {
  return path.join(resolveActiveKgDir(getConfig()), ".attribution.sqlite");
}

export function openAttributionStore(dbPath: string = defaultAttributionPath()): AttributionStore {
  fs.mkdirSync(path.dirname(dbPath), { recursive: true });
  const db = new Database(dbPath);
  db.pragma("journal_mode = WAL");
  db.exec(
    `CREATE TABLE IF NOT EXISTS source_attribution (
       utterance_id TEXT PRIMARY KEY,
       source       TEXT NOT NULL,
       source_url   TEXT NOT NULL,
       native_id    TEXT NOT NULL,
       game_slug    TEXT,
       imported_at  INTEGER NOT NULL
     )`
  );

  const setStmt = db.prepare(
    `INSERT INTO source_attribution (utterance_id, source, source_url, native_id, game_slug, imported_at)
       VALUES (@utteranceId, @source, @sourceUrl, @nativeId, @gameSlug, @importedAt)
     ON CONFLICT(utterance_id) DO UPDATE SET
       source = excluded.source, source_url = excluded.source_url,
       native_id = excluded.native_id, game_slug = excluded.game_slug,
       imported_at = excluded.imported_at`
  );
  const getStmt = db.prepare(
    "SELECT utterance_id, source, source_url, native_id, game_slug, imported_at FROM source_attribution WHERE utterance_id = ?"
  );

  type Row = {
    utterance_id: string;
    source: string;
    source_url: string;
    native_id: string;
    game_slug: string | null;
    imported_at: number;
  };
  const toAttribution = (row: Row): Attribution => ({
    utteranceId: row.utterance_id,
    source: row.source,
    sourceUrl: row.source_url,
    nativeId: row.native_id,
    gameSlug: row.game_slug,
    importedAt: row.imported_at,
  });

  return {
    set: (input) => {
      setStmt.run({
        utteranceId: input.utteranceId,
        source: String(input.source),
        sourceUrl: input.sourceUrl,
        nativeId: input.nativeId,
        gameSlug: input.gameSlug ?? null,
        importedAt: input.at ?? Date.now(),
      });
    },
    get: (utteranceId) => {
      const row = getStmt.get(utteranceId) as Row | undefined;
      return row ? toAttribution(row) : null;
    },
    listBySource: (source, gameSlug, limit = 300) => {
      const rows = (
        gameSlug
          ? db
              .prepare(
                "SELECT * FROM source_attribution WHERE source = ? AND game_slug = ? ORDER BY imported_at DESC LIMIT ?"
              )
              .all(source, gameSlug, limit)
          : db
              .prepare("SELECT * FROM source_attribution WHERE source = ? ORDER BY imported_at DESC LIMIT ?")
              .all(source, limit)
      ) as Row[];
      return rows.map(toAttribution);
    },
    countsBySource: () => {
      const rows = db
        .prepare(
          `SELECT source, COUNT(*) AS count, MAX(imported_at) AS last_imported_at,
                  GROUP_CONCAT(DISTINCT game_slug) AS game_slugs
             FROM source_attribution
            GROUP BY source
            ORDER BY count DESC`
        )
        .all() as Array<{
        source: string;
        count: number;
        last_imported_at: number | null;
        game_slugs: string | null;
      }>;
      return rows.map((r) => ({
        source: r.source,
        count: r.count,
        gameSlugs: (r.game_slugs ?? "")
          .split(",")
          .map((s) => s.trim())
          .filter((s) => s.length > 0),
        lastImportedAt: r.last_imported_at,
      }));
    },
    close: () => db.close(),
  };
}
