/**
 * フロー DB コネクション。
 *
 * discutere.db (DATABASE_PATH) と同じ SQLite ファイルを使う。
 * better-sqlite3 インスタンスを直接返し、flow マイグレーションを適用済みの状態にする。
 */

import Database from "better-sqlite3";
import fs from "node:fs";
import path from "node:path";
import { applyFlowMigrations } from "./migrations.js";

let instance: Database.Database | null = null;

export function getFlowDb(): Database.Database {
  if (instance) return instance;

  const dbPath = process.env.DATABASE_PATH ?? "./data/discutere.db";
  const dir = path.dirname(path.resolve(dbPath));
  if (!fs.existsSync(dir)) {
    fs.mkdirSync(dir, { recursive: true });
  }

  const db = new Database(dbPath);
  db.pragma("journal_mode = WAL");
  db.pragma("foreign_keys = ON");
  applyFlowMigrations(db);
  instance = db;
  return db;
}

/** テスト用: インスタンスをリセットして新しい DB ファイルで再初期化できるようにする */
export function _resetFlowDb(): void {
  instance = null;
}
