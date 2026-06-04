import assert from "node:assert/strict";
import Database from "better-sqlite3";

import {
  buildLearningLayerSnapshot,
  normalizeLearningLayer,
} from "../../src/visualize/learning-layers.js";

const db = new Database(":memory:");
db.exec(`
  CREATE TABLE games (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, genre TEXT, updated_at INTEGER);
  CREATE TABLE mechanics (id TEXT PRIMARY KEY, workspace_id TEXT, game_id TEXT, name TEXT, description TEXT, intends TEXT, intended_affect TEXT, updated_at INTEGER);
  CREATE TABLE aesthetics (id TEXT PRIMARY KEY, workspace_id TEXT, game_id TEXT, name TEXT, description TEXT, updated_at INTEGER);
  CREATE TABLE affects (id TEXT PRIMARY KEY, workspace_id TEXT, mood TEXT);
  CREATE TABLE design_gaps (id TEXT PRIMARY KEY, workspace_id TEXT, game_id TEXT, title TEXT, description TEXT, status TEXT, gap_in TEXT, expected_affect TEXT, observed_affect TEXT, updated_at INTEGER);
  CREATE TABLE hypotheses (id TEXT PRIMARY KEY, workspace_id TEXT, design_gap_id TEXT, statement TEXT, status TEXT, integrated INTEGER, updated_at INTEGER);
  CREATE TABLE sessions (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, scene TEXT);
  CREATE TABLE utterances (id TEXT PRIMARY KEY, workspace_id TEXT, session_id TEXT, raw_content TEXT);
`);

const ws = "knowledge";
db.prepare("INSERT INTO games VALUES (?,?,?,?,?)").run("game-1", ws, "Sample Game", "action", 100);
db.prepare("INSERT INTO mechanics VALUES (?,?,?,?,?,?,?,?)").run(
  "m1",
  ws,
  "game-1",
  "Dash",
  "fast movement",
  "make traversal confident",
  "accomplishment",
  120
);
db.prepare("INSERT INTO mechanics VALUES (?,?,?,?,?,?,?,?)").run(
  "m2",
  ws,
  "game-1",
  "Reward Loop",
  null,
  null,
  "accomplishment",
  110
);
db.prepare("INSERT INTO aesthetics VALUES (?,?,?,?,?,?)").run("a1", ws, "game-1", "Speed", null, 105);
db.prepare("INSERT INTO affects VALUES (?,?,?)").run("af1", ws, "accomplishment");
db.prepare("INSERT INTO design_gaps VALUES (?,?,?,?,?,?,?,?,?,?)").run(
  "g1",
  ws,
  "game-1",
  "Dash feels unclear",
  null,
  "open",
  "m1",
  "accomplishment",
  "confusion",
  130
);
db.prepare("INSERT INTO hypotheses VALUES (?,?,?,?,?,?,?)").run(
  "h1",
  ws,
  "g1",
  "Add a stronger dash trail",
  "proposed",
  0,
  140
);

assert.equal(normalizeLearningLayer("games"), "games");
assert.equal(normalizeLearningLayer("bad"), "knowledge");

const knowledge = buildLearningLayerSnapshot(db, ws, "knowledge");
assert.equal(knowledge.layer, "knowledge");
assert.equal(knowledge.totals.games, 1);
assert.equal(knowledge.totals.mechanics, 2);
assert.ok(knowledge.affects.some((affect) => affect.key === "accomplishment" && affect.usageCount >= 2));

const games = buildLearningLayerSnapshot(db, ws, "games");
assert.equal(games.layer, "games");
assert.equal(games.games[0].title, "Sample Game");
assert.equal(games.games[0].mechanicCount, 2);
assert.equal(games.games[0].aestheticCount, 1);
assert.equal(games.games[0].opinionCount, 1);

const opinions = buildLearningLayerSnapshot(db, ws, "opinions");
assert.equal(opinions.layer, "opinions");
assert.equal(opinions.topics[0].opinions[0].statement, "Add a stronger dash trail");

db.close();

console.log("learning-layers.test.ts: all passed");
