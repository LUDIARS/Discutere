import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import Database from "better-sqlite3";

import {
  planQuarantine,
  quarantineSource,
  restoreQuarantine,
  listArchives,
} from "../../src/crawler/quarantine.js";

const ws = "knowledge";

function makeKg(): Database.Database {
  const db = new Database(":memory:");
  db.exec(`
    CREATE TABLE sessions (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, started_at INTEGER, ended_at INTEGER, created_at INTEGER, updated_at INTEGER, mode TEXT, scene TEXT);
    CREATE TABLE utterances (id TEXT PRIMARY KEY, workspace_id TEXT, session_id TEXT, speaker_id TEXT, raw_content TEXT, normalized_content TEXT, posted_at INTEGER, created_at INTEGER, updated_at INTEGER, responds_to TEXT);
    CREATE TABLE reactions (id TEXT PRIMARY KEY, workspace_id TEXT, utterance_id TEXT, actor_id TEXT, reaction_type TEXT, intensity REAL, created_at INTEGER, updated_at INTEGER);
    CREATE TABLE translation_proposals (id TEXT PRIMARY KEY, workspace_id TEXT, utterance_id TEXT, proposal_type TEXT, target_id TEXT, target_name TEXT, confidence REAL, pending_review INTEGER, status TEXT, payload_json TEXT, created_at INTEGER, updated_at INTEGER);
    CREATE TABLE embeddings (id TEXT PRIMARY KEY, workspace_id TEXT, node_type TEXT, node_id TEXT, vector_json TEXT, created_at INTEGER);
    CREATE TABLE opinion_scores (target_id TEXT PRIMARY KEY, target_kind TEXT, score INTEGER, updated_at INTEGER);
    CREATE TABLE discord_message_map (message_id TEXT PRIMARY KEY, target_id TEXT, target_kind TEXT, channel_id TEXT, created_at INTEGER);
    CREATE TABLE affects (id TEXT PRIMARY KEY, workspace_id TEXT, session_id TEXT, subject_id TEXT, mood TEXT, score REAL, created_at INTEGER, updated_at INTEGER, vocabulary_status TEXT, valence TEXT);
  `);
  return db;
}

const sess = (db: Database.Database, id: string, title: string) =>
  db.prepare("INSERT INTO sessions (id,workspace_id,title,started_at,created_at,updated_at) VALUES (?,?,?,0,0,0)").run(id, ws, title);
const utt = (db: Database.Database, id: string, sid: string) =>
  db.prepare("INSERT INTO utterances (id,workspace_id,session_id,raw_content,posted_at,created_at,updated_at) VALUES (?,?,?,?,0,0,0)").run(id, ws, sid, "x");

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "di-quar-"));
const archiveDir = path.join(tmp, "quarantine");
const ingestedPath = path.join(tmp, "ingested.sqlite");

// ingested-store 準備 (website 2 行 + steam 1 行)
const idb = new Database(ingestedPath);
idb.exec("CREATE TABLE ingested (source TEXT NOT NULL, native_id TEXT NOT NULL, imported_at INTEGER NOT NULL, PRIMARY KEY (source, native_id))");
idb.prepare("INSERT INTO ingested VALUES (?,?,0)").run("website", "https://a");
idb.prepare("INSERT INTO ingested VALUES (?,?,0)").run("website", "https://b");
idb.prepare("INSERT INTO ingested VALUES (?,?,0)").run("steam", "111");
idb.close();

const db = makeKg();
// website × wonda: 2 session / 2 utterance / 1 reaction
sess(db, "w1", "website:wonda");
sess(db, "w2", "website:wonda");
utt(db, "wu1", "w1");
utt(db, "wu2", "w2");
db.prepare("INSERT INTO reactions (id,workspace_id,utterance_id,reaction_type,created_at,updated_at) VALUES (?,?,?,?,0,0)").run("r1", ws, "wu1", "up");
db.prepare("INSERT INTO embeddings VALUES (?,?,?,?,?,0)").run("e1", ws, "utterance", "wu1", "[]");
// youtube × wonda: 残す側 (隔離対象外)
sess(db, "y1", "youtube:wonda");
utt(db, "yu1", "y1");

// --- dry-run plan ---
const plan = planQuarantine(db, ws, "website", "wonda");
assert.equal(plan.counts.sessions, 2, "plan sessions");
assert.equal(plan.counts.utterances, 2, "plan utterances");
assert.equal(plan.counts.reactions, 1, "plan reactions");
assert.equal(plan.counts.embeddings, 1, "plan embeddings");

// --- quarantine (website×wonda; slug 指定 → ingested は触らない) ---
const res = quarantineSource(db, ws, "website", "wonda", { archiveDir, ingestedPath, now: 1000 });
assert.ok(fs.existsSync(res.archiveFile), "archive file created");
assert.equal(db.prepare("SELECT COUNT(*) c FROM sessions WHERE title='website:wonda'").get().c, 0, "wonda website sessions gone");
assert.equal(db.prepare("SELECT COUNT(*) c FROM utterances WHERE session_id IN ('w1','w2')").get().c, 0, "utterances gone");
assert.equal(db.prepare("SELECT COUNT(*) c FROM reactions").get().c, 0, "reactions gone");
assert.equal(db.prepare("SELECT COUNT(*) c FROM sessions WHERE title='youtube:wonda'").get().c, 1, "youtube session kept");
// slug 指定なので ingested は無傷
const idb2 = new Database(ingestedPath, { readonly: true });
assert.equal(idb2.prepare("SELECT COUNT(*) c FROM ingested WHERE source='website'").get().c, 2, "ingested website kept (slug-scoped)");
idb2.close();

// --- list ---
const archives = listArchives(archiveDir);
assert.equal(archives.length, 1, "one archive listed");
assert.equal(archives[0].source, "website");
assert.equal(archives[0].counts.utterances, 2);

// --- restore ---
const restored = restoreQuarantine(db, res.archiveFile, { ingestedPath });
assert.equal(restored.counts.sessions, 2, "restored sessions");
assert.equal(restored.counts.utterances, 2, "restored utterances");
assert.equal(db.prepare("SELECT COUNT(*) c FROM sessions WHERE title='website:wonda'").get().c, 2, "wonda back");
assert.equal(db.prepare("SELECT COUNT(*) c FROM reactions").get().c, 1, "reaction back");

// --- whole-source quarantine purges ingested too ---
const res2 = quarantineSource(db, ws, "website", null, { archiveDir, ingestedPath, now: 2000 });
assert.equal(res2.counts.ingested, 2, "whole-source quarantine archives ingested rows");
const idb3 = new Database(ingestedPath, { readonly: true });
assert.equal(idb3.prepare("SELECT COUNT(*) c FROM ingested WHERE source='website'").get().c, 0, "ingested website purged");
assert.equal(idb3.prepare("SELECT COUNT(*) c FROM ingested WHERE source='steam'").get().c, 1, "steam ingested kept");
idb3.close();

db.close();
fs.rmSync(tmp, { recursive: true, force: true });
console.log("quarantine test: ok");
