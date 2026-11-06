// src/queue/snapshot.ts の入出力を記録する。実行: npx tsx native/tests/golden/w1-A-queue.gen.ts
// 合成の SQLite (DDL は src/core/db/schema.ts / persona-engine migrations の写し) で buildQueueSnapshot を呼ぶ。
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";
import { buildQueueSnapshot, formatQueueText } from "../../../src/queue/snapshot.js";

const coreDdl = [
  "CREATE TABLE sessions (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,title TEXT NOT NULL,started_at INTEGER NOT NULL,ended_at INTEGER,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL, scene TEXT)",
  "CREATE TABLE utterances (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,session_id TEXT NOT NULL,speaker_id TEXT,raw_content TEXT NOT NULL,normalized_content TEXT,posted_at INTEGER NOT NULL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)",
  "CREATE TABLE design_gaps (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,game_id TEXT,title TEXT NOT NULL,description TEXT,status TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)",
  "CREATE TABLE hypotheses (id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,design_gap_id TEXT,statement TEXT NOT NULL,status TEXT,integrated INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)",
];
const peDdl = ["CREATE TABLE session_rule_fires (session_id TEXT NOT NULL, rule_id TEXT NOT NULL, count INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (session_id, rule_id))"];
const coreRows: string[] = [];
for (let i = 0; i < 12; i++) {
  const scene = i === 0 ? "discord:g1/" : i === 1 ? "discord:/c1" : i === 2 ? "web:room" : `discord:g${i}/c${i}`;
  const ended = i === 3 ? 5 : "NULL";
  coreRows.push(`INSERT INTO sessions VALUES ('s${i}','ws','title${i}',${100 + i},${ended},0,0,'${scene}')`);
  for (let j = 0; j < i % 4; j++) coreRows.push(`INSERT INTO utterances VALUES ('u${i}-${j}','ws','s${i}',NULL,'x',NULL,${1000 + i * 10 + j},0,0)`);
}
coreRows.push("INSERT INTO sessions VALUES ('other','ws2','t',1,NULL,0,0,'discord:g/c')");
const statuses = ["open", null, "resolved", "closed", "rejected", "converged", "dismissed", "proposed"];
statuses.forEach((s, i) => coreRows.push(`INSERT INTO design_gaps VALUES ('g${i}','ws',NULL,'${"長いタイトル".repeat(i * 3)}DG${i}',NULL,${s === null ? "NULL" : `'${s}'`},${i},0)`));
const hyp = [["open", 0], [null, 0], ["rejected", 0], ["integrated", 0], ["testing", 1], ["pending", 0]] as const;
hyp.forEach(([s, integ], i) => coreRows.push(`INSERT INTO hypotheses VALUES ('h${i}','ws',${i % 2 ? "'g1'" : "NULL"},'仮説${"あ".repeat(i * 20)}',${s === null ? "NULL" : `'${s}'`},${integ},${i},0)`));
const peRows = ["INSERT INTO session_rule_fires VALUES ('s5','r1',2)", "INSERT INTO session_rule_fires VALUES ('s5','r2',3)", "INSERT INTO session_rule_fires VALUES ('s6','r1',1)"];

function run(withPe: boolean, workspace: string) {
  const core = new Database(":memory:");
  for (const s of [...coreDdl, ...coreRows]) core.exec(s);
  const pe = withPe ? new Database(":memory:") : null;
  if (pe) for (const s of [...peDdl, ...peRows]) pe.exec(s);
  const snap = buildQueueSnapshot(core, pe, workspace);
  snap.generatedAt = 0;
  return { withPe, workspace, snapshot: snap, text: formatQueueText(snap) };
}
const out = { coreSql: [...coreDdl, ...coreRows], peSql: [...peDdl, ...peRows], cases: [run(true, "ws"), run(false, "ws"), run(true, "empty")] };
const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "queue");
fs.writeFileSync(path.join(dir, "snapshot.json"), JSON.stringify(out, null, 2) + "\n");
console.log("wrote queue snapshot cases");
