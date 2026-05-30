/**
 * SessionFireStore (restart-recovery) テスト.
 *
 * - InMemory / Sqlite 両 store の increment / total / ruleCount / reset
 * - **再起動再現**: 同一 db ファイルに対して新しい SqliteSessionFireStore を作り直し、
 *   カウントが復元される (= 再起動越しに safety cap が効く) ことを確認。
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

import { applyPersonaEngineMigrations } from "../../src/persona-engine/db/migrations.js";
import {
  InMemorySessionFireStore,
  SqliteSessionFireStore,
  type SessionFireStore,
} from "../../src/persona-engine/db/session-fires-repo.js";

function runStoreContract(label: string, make: () => SessionFireStore): void {
  const store = make();
  assert.equal(store.total("s1"), 0, `${label}: empty total`);
  assert.equal(store.ruleCount("s1", "r1"), 0, `${label}: empty ruleCount`);

  store.increment("s1", "r1");
  store.increment("s1", "r1");
  store.increment("s1", "r2");
  assert.equal(store.ruleCount("s1", "r1"), 2, `${label}: r1 count`);
  assert.equal(store.ruleCount("s1", "r2"), 1, `${label}: r2 count`);
  assert.equal(store.total("s1"), 3, `${label}: total`);

  // 別 session は独立
  store.increment("s2", "r1");
  assert.equal(store.total("s2"), 1, `${label}: s2 independent`);
  assert.equal(store.total("s1"), 3, `${label}: s1 unaffected by s2`);

  // reset は対象 session のみ消す
  store.reset("s1");
  assert.equal(store.total("s1"), 0, `${label}: s1 reset`);
  assert.equal(store.total("s2"), 1, `${label}: s2 survives s1 reset`);
  console.log(`ok ${label} store contract`);
}

runStoreContract("in-memory", () => new InMemorySessionFireStore());

const workDir = path.resolve(".tmp/pe-session-fires");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });
const dbPath = path.join(workDir, "fires.db");

{
  const db = new Database(dbPath);
  applyPersonaEngineMigrations(db);
  runStoreContract("sqlite", () => new SqliteSessionFireStore(db));
  db.close();
}

// ─── 再起動再現: 同 db を開き直してもカウントが残る ─────────────
{
  const db1 = new Database(dbPath);
  applyPersonaEngineMigrations(db1);
  const store1 = new SqliteSessionFireStore(db1);
  store1.reset("live"); // 前段の汚染を除去
  store1.increment("live", "ruleX");
  store1.increment("live", "ruleX");
  store1.increment("live", "ruleY");
  assert.equal(store1.total("live"), 3, "pre-restart total");
  db1.close(); // = プロセス終了相当

  // 「再起動」: 新しい接続 + 新しい store
  const db2 = new Database(dbPath);
  applyPersonaEngineMigrations(db2);
  const store2 = new SqliteSessionFireStore(db2);
  assert.equal(store2.total("live"), 3, "counts survive restart");
  assert.equal(store2.ruleCount("live", "ruleX"), 2, "per-rule count survives restart");
  assert.equal(store2.ruleCount("live", "ruleY"), 1, "per-rule count survives restart (Y)");

  // 再起動後も increment が連続する (0 から数え直さない)
  store2.increment("live", "ruleX");
  assert.equal(store2.ruleCount("live", "ruleX"), 3, "increment continues post-restart");
  db2.close();
  console.log("ok counts survive restart (cap holds across reboot)");
}

console.log("session-fire-store.test.ts: all passed");
