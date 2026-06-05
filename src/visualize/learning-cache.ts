/**
 * Learning View 読み取りキャッシュ。
 *
 * 学習ビューア (`/learning*`) が参照する各クエリ結果を、KG (Kuzu/Core) から 1 回だけ
 * 実行して別 SQLite (`data/learning-cache.sqlite`) に JSON で固める。ビューア側はこの
 * キャッシュだけを読むので、KG (単一 writer) をロックせず、Bot 稼働中でも参照できる。
 *
 * 「収束した論述データは一度作ったら変わらない」前提のため、データ追加・議論収束の後に
 * `scripts/build-learning-cache.ts` で作り直す運用。
 */

import fs from "node:fs";
import path from "node:path";

import Database from "better-sqlite3";

import type { createCore } from "../core/index.js";
import { buildLearningLayerSnapshot, type LearningLayer } from "./learning-layers.js";
import { listConclusions, getConclusionDetail } from "./conclusions.js";

type Core = ReturnType<typeof createCore>;

export const DEFAULT_CACHE_PATH = path.resolve("./data/learning-cache.sqlite");

/** ビューアが /learning/data で引く層 (conclusions は別エンドポイント)。 */
const CACHED_LAYERS: LearningLayer[] = ["knowledge", "games", "opinions"];
/** ビューア HTML が投げるクエリと同じ上限値で焼く。 */
const CONCLUSION_LIMIT = 200;
const LAYER_LIMIT = 80;
const LAYER_DETAIL_LIMIT = 8;

const META_KEY = "__meta__";

function openDb(dbPath: string): Database.Database {
  fs.mkdirSync(path.dirname(dbPath), { recursive: true });
  const db = new Database(dbPath);
  db.pragma("journal_mode = WAL");
  db.exec(
    "CREATE TABLE IF NOT EXISTS cache (key TEXT PRIMARY KEY, payload TEXT NOT NULL, built_at INTEGER NOT NULL)"
  );
  return db;
}

export interface BuildLearningCacheResult {
  path: string;
  builtAt: number;
  conclusions: number;
  details: number;
  layers: number;
}

/**
 * KG から全クエリを実行してキャッシュ SQLite を作り直す。`now` はテスト用に注入可能。
 */
export function buildLearningCache(
  core: Core,
  workspaceId: string,
  dbPath: string = DEFAULT_CACHE_PATH,
  now: number = Date.now()
): BuildLearningCacheResult {
  const db = openDb(dbPath);
  try {
    const put = db.prepare(
      "INSERT INTO cache (key, payload, built_at) VALUES (?, ?, ?) " +
        "ON CONFLICT(key) DO UPDATE SET payload = excluded.payload, built_at = excluded.built_at"
    );
    const run = db.transaction(() => {
      // 結論詳細は件数が変動するので毎回作り直す (古い gap の残骸を残さない)。
      db.prepare("DELETE FROM cache WHERE key LIKE 'conclusion:%'").run();

      const conclusions = listConclusions(core, workspaceId, CONCLUSION_LIMIT);
      put.run("conclusions", JSON.stringify({ conclusions }), now);

      let details = 0;
      for (const c of conclusions) {
        const detail = getConclusionDetail(core, workspaceId, c.gapId);
        if (detail) {
          put.run(`conclusion:${c.gapId}`, JSON.stringify(detail), now);
          details += 1;
        }
      }

      let layers = 0;
      for (const layer of CACHED_LAYERS) {
        const snapshot = buildLearningLayerSnapshot(core.client.raw, workspaceId, layer, {
          limit: LAYER_LIMIT,
          detailLimit: LAYER_DETAIL_LIMIT,
        });
        put.run(`layer:${layer}`, JSON.stringify(snapshot), now);
        layers += 1;
      }

      put.run(META_KEY, JSON.stringify({ builtAt: now, workspaceId }), now);
      return { conclusions: conclusions.length, details, layers };
    });
    const counts = run();
    return { path: dbPath, builtAt: now, ...counts };
  } finally {
    db.close();
  }
}

export interface LearningCacheReader {
  conclusions(): unknown;
  conclusion(gapId: string): unknown | undefined;
  layer(name: string): unknown | undefined;
  builtAt(): number | null;
  close(): void;
}

/** 読み取り専用でキャッシュを開く。KG には一切触れない。 */
export function openLearningCacheReader(dbPath: string = DEFAULT_CACHE_PATH): LearningCacheReader {
  const db = new Database(dbPath, { readonly: true, fileMustExist: true });
  const get = db.prepare("SELECT payload FROM cache WHERE key = ?");
  const read = (key: string): unknown | undefined => {
    const row = get.get(key) as { payload: string } | undefined;
    return row ? (JSON.parse(row.payload) as unknown) : undefined;
  };
  return {
    conclusions: () => read("conclusions") ?? { conclusions: [] },
    conclusion: (gapId) => read(`conclusion:${gapId}`),
    layer: (name) => read(`layer:${name}`),
    builtAt: () => {
      const meta = read(META_KEY) as { builtAt?: number } | undefined;
      return meta?.builtAt ?? null;
    },
    close: () => db.close(),
  };
}

export function learningCacheExists(dbPath: string = DEFAULT_CACHE_PATH): boolean {
  return fs.existsSync(dbPath);
}
