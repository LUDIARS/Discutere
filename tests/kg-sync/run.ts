/**
 * KG 共有同期の単体テスト — spec/core/KG-SYNC.md。
 * export → import の round-trip / 共有フラグ絞り込み / LWW / whitelist 拒否 を確認する。
 * 実 DB (in-memory better-sqlite3) を使い、ネットワークには触れない。
 */

import Database from "better-sqlite3";

import { applyMigrations } from "../../src/core/db/schema.js";
import { exportSharedKg } from "../../src/kg-sync/export.js";
import { importSharedKg } from "../../src/kg-sync/import.js";

let failed = 0;
function ok(cond: boolean, msg: string): void {
  if (cond) console.log(`ok ${msg}`);
  else {
    console.error(`FAIL ${msg}`);
    failed++;
  }
}

function freshDb(): Database.Database {
  const db = new Database(":memory:");
  applyMigrations(db);
  return db;
}

function insertGame(
  db: Database.Database,
  args: { id: string; title: string; updatedAt: number; shared: number }
): void {
  db.prepare(
    `INSERT INTO games (id, workspace_id, title, genre, created_at, updated_at, shared) VALUES (?, ?, ?, ?, ?, ?, ?)`
  ).run(args.id, "knowledge", args.title, null, args.updatedAt, args.updatedAt, args.shared);
}

// ── master を用意: 共有 2 件 + 非共有 1 件 ──
const master = freshDb();
insertGame(master, { id: "g-shared-1", title: "Vampire Survivors", updatedAt: 1000, shared: 1 });
insertGame(master, { id: "g-shared-2", title: "Hades", updatedAt: 2000, shared: 1 });
insertGame(master, { id: "g-private", title: "社内メモ", updatedAt: 3000, shared: 0 });
master
  .prepare(
    `INSERT INTO mechanics (id, workspace_id, game_id, name, description, created_at, updated_at, shared) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
  )
  .run("m-shared-1", "knowledge", "g-shared-1", "経験値磁石", null, 1500, 1500, 1);

// ── export(since=0) ──
const full = exportSharedKg(master, { since: 0, workspaceId: "knowledge" });
ok(full.count === 3, `共有 3 件 export (got ${full.count})`);
ok(full.watermark === 2000, `watermark = 最大共有 updated_at 2000 (got ${full.watermark})`);
ok(!full.ndjson.includes("g-private"), "非共有行は export に含まれない");

// ── follower へ import ──
const follower = freshDb();
const imp = importSharedKg(follower, full.ndjson);
ok(imp.applied === 3, `3 件 applied (got ${imp.applied})`);
const followerGames = follower.prepare("SELECT id FROM games ORDER BY id").all() as Array<{ id: string }>;
ok(followerGames.length === 2, `follower games は共有 2 件のみ (got ${followerGames.length})`);
ok(
  followerGames.some((r) => r.id === "g-shared-1") && !followerGames.some((r) => r.id === "g-private"),
  "共有行のみ反映 / 非共有は来ない"
);
ok(
  (follower.prepare("SELECT title FROM games WHERE id=?").get("g-shared-2") as { title: string }).title === "Hades",
  "値が正しく転送される"
);

// ── 増分: since=watermark なら 0 件 ──
const incr = exportSharedKg(master, { since: full.watermark, workspaceId: "knowledge" });
ok(incr.count === 0, `since=watermark で増分 0 件 (got ${incr.count})`);

// ── LWW: follower の方が新しい行は古い更新で上書きしない ──
insertGame(follower, { id: "g-lww", title: "follower-new", updatedAt: 5000, shared: 1 });
const stale = JSON.stringify({ type: "row", table: "games", data: {
  id: "g-lww", workspace_id: "knowledge", title: "master-old", genre: null,
  created_at: 100, updated_at: 100, shared: 1,
} });
const lwwOld = importSharedKg(follower, stale);
ok(lwwOld.skipped === 1 && lwwOld.applied === 0, "古い更新は LWW で skip");
ok(
  (follower.prepare("SELECT title FROM games WHERE id=?").get("g-lww") as { title: string }).title === "follower-new",
  "LWW: 新しい既存値が保持される"
);
const fresh = JSON.stringify({ type: "row", table: "games", data: {
  id: "g-lww", workspace_id: "knowledge", title: "master-new", genre: null,
  created_at: 100, updated_at: 9000, shared: 1,
} });
importSharedKg(follower, fresh);
ok(
  (follower.prepare("SELECT title FROM games WHERE id=?").get("g-lww") as { title: string }).title === "master-new",
  "LWW: より新しい更新は上書きされる"
);

// ── whitelist 拒否: 共有外テーブルへの注入は無視 ──
const forged = JSON.stringify({ type: "row", table: "sessions", data: {
  id: "s-evil", workspace_id: "knowledge", title: "x", started_at: 1, created_at: 1, updated_at: 1,
} });
const forgedRes = importSharedKg(follower, forged);
ok(forgedRes.ignored === 1 && forgedRes.applied === 0, "whitelist 外テーブルは ignored");
ok((follower.prepare("SELECT COUNT(*) c FROM sessions").get() as { c: number }).c === 0, "sessions に注入されない");

master.close();
follower.close();

if (failed > 0) {
  console.error(`\n${failed} test(s) failed`);
  process.exit(1);
}
console.log("\nall kg-sync tests passed");
