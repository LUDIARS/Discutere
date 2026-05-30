/**
 * 議論キュー snapshot の単体テスト (in-memory sqlite)。
 * buildQueueSnapshot が discord-bound active session / 未処理 gap / 検証待ち仮説を
 * 正しく拾い、ended / resolved / integrated を除外することを確認する。
 */

import Database from "better-sqlite3";
import { buildQueueSnapshot, formatQueueText } from "../../src/queue/snapshot.js";

let failed = 0;
function ok(cond: boolean, msg: string): void {
  if (cond) console.log(`ok ${msg}`);
  else {
    console.error(`FAIL ${msg}`);
    failed++;
  }
}

function makeCoreDb(): Database.Database {
  const db = new Database(":memory:");
  db.exec(`
    CREATE TABLE sessions (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, scene TEXT, started_at INTEGER, ended_at INTEGER);
    CREATE TABLE utterances (id TEXT PRIMARY KEY, workspace_id TEXT, session_id TEXT, raw_content TEXT, posted_at INTEGER);
    CREATE TABLE design_gaps (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, status TEXT, created_at INTEGER);
    CREATE TABLE hypotheses (id TEXT PRIMARY KEY, workspace_id TEXT, design_gap_id TEXT, statement TEXT, status TEXT, integrated INTEGER, created_at INTEGER);
  `);
  return db;
}

const WS = "knowledge";
const core = makeCoreDb();

// active discord session
core.prepare("INSERT INTO sessions VALUES (?,?,?,?,?,?)").run("s1", WS, "discord-session:g1:c1", "discord:g1/c1", 100, null);
// ended session — 除外される
core.prepare("INSERT INTO sessions VALUES (?,?,?,?,?,?)").run("s2", WS, "discord-session:g1:c2", "discord:g1/c2", 90, 200);
// gap-only session (非 discord) — 除外
core.prepare("INSERT INTO sessions VALUES (?,?,?,?,?,?)").run("s3", WS, "discussion-of-gap:x", "gap:x", 80, null);

core.prepare("INSERT INTO utterances VALUES (?,?,?,?,?)").run("u1", WS, "s1", "hello", 150);
core.prepare("INSERT INTO utterances VALUES (?,?,?,?,?)").run("u2", WS, "s1", "world", 160);

core.prepare("INSERT INTO design_gaps VALUES (?,?,?,?,?)").run("g1", WS, "open gap", null, 100);
core.prepare("INSERT INTO design_gaps VALUES (?,?,?,?,?)").run("g2", WS, "resolved gap", "resolved", 100);

core.prepare("INSERT INTO hypotheses VALUES (?,?,?,?,?,?,?)").run("h1", WS, "g1", "maybe X", null, 0, 100);
core.prepare("INSERT INTO hypotheses VALUES (?,?,?,?,?,?,?)").run("h2", WS, "g1", "integrated one", "integrated", 1, 100);

// persona-engine fire store
const pe = new Database(":memory:");
pe.exec("CREATE TABLE session_rule_fires (session_id TEXT, rule_id TEXT, count INTEGER, PRIMARY KEY(session_id, rule_id))");
pe.prepare("INSERT INTO session_rule_fires VALUES (?,?,?)").run("s1", "r1", 3);

const snap = buildQueueSnapshot(core, pe, WS);

ok(snap.totals.activeSessions === 1, "active discord session のみ拾う (ended/gap 除外)");
ok(snap.activeSessions[0]?.channelId === "c1", "scene から channelId を抽出");
ok(snap.activeSessions[0]?.utteranceCount === 2, "発話数を集計");
ok(snap.activeSessions[0]?.fireCount === 3, "persona-engine 発火数を集計");
ok(snap.totals.openGaps === 1 && snap.openGaps[0]?.id === "g1", "未解決 gap のみ (resolved 除外)");
ok(snap.totals.pendingHypotheses === 1 && snap.pendingHypotheses[0]?.id === "h1", "未統合仮説のみ (integrated 除外)");
ok(formatQueueText(snap).includes("議論キュー"), "formatQueueText がサマリ文字列を返す");

core.close();
pe.close();

if (failed > 0) {
  console.error(`queue tests: ${failed} failed`);
  process.exit(1);
}
console.log("queue tests: all passed");
