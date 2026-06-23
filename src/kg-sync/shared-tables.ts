/**
 * KG 共有同期で扱う「共有資産」テーブルの定義 — spec/data/core/KG-SYNC.md。
 *
 * master が `shared=1` を立てた行だけが follower へ配布される (学習知識のみ。
 * 議論ローカルデータ = sessions / utterances / design_gaps 等は同期しない)。
 *
 * 列はハードコードせず実行時に PRAGMA table_info で取得する (スキーマ追加に追従し、
 * master/follower で列差があっても共通部分だけを同期できる)。
 *
 * Phase 1 は SQLite 上の学習知識 4 テーブル。embeddings (updated_at 無し・再生成可) と
 * 外部発話 (Kuzu/sidecar 側) は Phase 2 (KG-SYNC.md §6)。
 */

import type Database from "better-sqlite3";

/** NDJSON フォーマットのスキーマ版。互換性が壊れる変更時に上げる。 */
export const KG_SYNC_SCHEMA_VERSION = 1;

/** 同期対象テーブル (whitelist)。import 側はここに無いテーブルを拒否する (任意行注入の防止)。 */
export const SHARED_TABLES = ["games", "mechanics", "aesthetics", "affects"] as const;
export type SharedTable = (typeof SHARED_TABLES)[number];

/** 透かし列 (増分の基準)。全共有テーブルが updated_at を持つ。 */
export const WATERMARK_COLUMN = "updated_at";

/** 共有フラグ列。master 側で 1 の行だけが配布対象。 */
export const SHARED_FLAG_COLUMN = "shared";

export function isSharedTable(name: string): name is SharedTable {
  return (SHARED_TABLES as readonly string[]).includes(name);
}

const columnCache = new WeakMap<Database.Database, Map<string, string[]>>();

/** テーブルの実列名を PRAGMA で取得 (DB ごとにキャッシュ)。存在しなければ throw。 */
export function tableColumns(db: Database.Database, table: string): string[] {
  let perDb = columnCache.get(db);
  if (!perDb) {
    perDb = new Map();
    columnCache.set(db, perDb);
  }
  const cached = perDb.get(table);
  if (cached) return cached;
  const rows = db.prepare(`PRAGMA table_info(${table})`).all() as Array<{ name: string }>;
  if (rows.length === 0) throw new Error(`tableColumns: テーブルが存在しません: ${table}`);
  const cols = rows.map((r) => r.name);
  perDb.set(table, cols);
  return cols;
}
