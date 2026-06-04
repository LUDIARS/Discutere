import assert from "node:assert/strict";
import Database from "better-sqlite3";

import { buildTopicOpinionSnapshot } from "../../src/visualize/topic-opinions.js";

const db = new Database(":memory:");
db.exec(`
  CREATE TABLE design_gaps (
    id TEXT PRIMARY KEY,
    workspace_id TEXT NOT NULL,
    title TEXT NOT NULL,
    description TEXT,
    status TEXT,
    gap_in TEXT,
    expected_affect TEXT,
    observed_affect TEXT,
    updated_at INTEGER NOT NULL
  );
  CREATE TABLE hypotheses (
    id TEXT PRIMARY KEY,
    workspace_id TEXT NOT NULL,
    design_gap_id TEXT,
    statement TEXT NOT NULL,
    status TEXT,
    integrated INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL
  );
  CREATE TABLE sessions (
    id TEXT PRIMARY KEY,
    workspace_id TEXT NOT NULL,
    title TEXT NOT NULL,
    scene TEXT
  );
  CREATE TABLE utterances (
    id TEXT PRIMARY KEY,
    workspace_id TEXT NOT NULL,
    session_id TEXT NOT NULL,
    raw_content TEXT NOT NULL
  );
`);

const ws = "knowledge";
db.prepare("INSERT INTO design_gaps VALUES (?,?,?,?,?,?,?,?,?)").run(
  "g-big",
  ws,
  "オンボーディングが長い",
  "初回体験の離脱が多い",
  "open",
  "m1",
  "confidence",
  "confusion",
  200
);
db.prepare("INSERT INTO design_gaps VALUES (?,?,?,?,?,?,?,?,?)").run(
  "g-small",
  ws,
  "報酬が弱い",
  null,
  "open",
  null,
  null,
  null,
  300
);

db.prepare("INSERT INTO hypotheses VALUES (?,?,?,?,?,?,?)").run(
  "h1",
  ws,
  "g-big",
  "最初の選択肢を二つに絞る",
  "proposed",
  0,
  220
);
db.prepare("INSERT INTO hypotheses VALUES (?,?,?,?,?,?,?)").run(
  "h2",
  ws,
  "g-big",
  "説明をプレイ中の短い提示へ移す",
  "integrated",
  1,
  230
);
db.prepare("INSERT INTO hypotheses VALUES (?,?,?,?,?,?,?)").run(
  "h3",
  ws,
  "g-small",
  "短期報酬を追加する",
  null,
  0,
  240
);

db.prepare("INSERT INTO sessions VALUES (?,?,?,?)").run(
  "s1",
  ws,
  "discussion-of-gap:g-big",
  "gap:g-big"
);
db.prepare("INSERT INTO utterances VALUES (?,?,?,?)").run("u1", ws, "s1", "agree");
db.prepare("INSERT INTO utterances VALUES (?,?,?,?)").run("u2", ws, "s1", "disagree");

const snap = buildTopicOpinionSnapshot(db, ws);

assert.equal(snap.totals.topics, 2);
assert.equal(snap.totals.opinions, 3);
assert.equal(snap.totals.utterances, 2);
assert.equal(snap.topics[0].id, "g-big");
assert.equal(snap.topics[0].size, 8);
assert.equal(snap.topics[0].opinions.length, 2);
assert.equal(snap.topics[0].opinions[0].id, "h2");
assert.equal(snap.topics[0].opinions[0].integrated, true);
assert.equal(snap.topics[1].size, 3);

db.close();

console.log("topic-opinions.test.ts: all passed");
