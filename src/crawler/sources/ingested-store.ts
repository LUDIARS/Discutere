/**
 * dedup sidecar — spec/feature/crawler/EXTERNAL-SOURCES.md §3 dedup.
 *
 * 取り込み済み (source, nativeId) を記録する小さな SQLite。utterances 本体には
 * 外部 id 列を足さない (schema 不変) ため、冪等性はこの sidecar が担保する。
 */

import fs from "node:fs";
import path from "node:path";

import Database from "better-sqlite3";

import type { ExternalSource } from "./types.js";

export interface IngestedStore {
  has(source: ExternalSource, nativeId: string): boolean;
  add(source: ExternalSource, nativeId: string, at?: number): void;
  close(): void;
}

export const DEFAULT_INGESTED_PATH = path.resolve("./data/external/.ingested.sqlite");

export function openIngestedStore(dbPath: string = DEFAULT_INGESTED_PATH): IngestedStore {
  fs.mkdirSync(path.dirname(dbPath), { recursive: true });
  const db = new Database(dbPath);
  db.pragma("journal_mode = WAL");
  db.exec(
    "CREATE TABLE IF NOT EXISTS ingested (source TEXT NOT NULL, native_id TEXT NOT NULL, imported_at INTEGER NOT NULL, PRIMARY KEY (source, native_id))"
  );
  const hasStmt = db.prepare("SELECT 1 FROM ingested WHERE source = ? AND native_id = ?");
  const addStmt = db.prepare(
    "INSERT OR IGNORE INTO ingested (source, native_id, imported_at) VALUES (?, ?, ?)"
  );
  return {
    has: (source, nativeId) => hasStmt.get(source, nativeId) !== undefined,
    add: (source, nativeId, at = Date.now()) => {
      addStmt.run(source, nativeId, at);
    },
    close: () => db.close(),
  };
}
