import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { createCore } from "../../../src/core/index.js";
import { importExternalUtterances } from "../../../src/crawler/sources/importer.js";
import { openIngestedStore } from "../../../src/crawler/sources/ingested-store.js";
import type { ExternalUtterance } from "../../../src/crawler/sources/types.js";

const workDir = path.resolve(".tmp/crawler-external");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
const ingested = openIngestedStore(path.join(workDir, "ingested.sqlite"));

// --- 1. Steam 風 (非スレッド, 賛否あり) ---
const steam: ExternalUtterance[] = [
  {
    source: "steam", nativeId: "r1", gameSlug: "vs", threadKey: "1794680",
    content: "面白い", postedAt: 1000, authorId: "S1", signal: { votedUp: true, upvotes: 5 },
    sourceUrl: "https://example/r1",
  },
  {
    source: "steam", nativeId: "r2", gameSlug: "vs", threadKey: "1794680",
    content: "重い", postedAt: 2000, authorId: "S2", signal: { votedUp: false },
    sourceUrl: "https://example/r2",
  },
];

const r1 = importExternalUtterances(core, steam, { workspaceId: "knowledge", ingested });
assert.equal(r1.sessions, 1, "appid 単位で 1 session");
assert.equal(r1.utterances, 2);
assert.equal(r1.reactions, 2);
assert.equal(r1.skipped, 0);

const u1 = core.client.raw.prepare("SELECT * FROM utterances WHERE id = ?").get("ext:steam:r1") as any;
assert.equal(u1.speaker_id, "ext:steam:S1", "公開 ID アンカーが speaker_id");
assert.equal(u1.session_id, "ext:steam:1794680");
assert.equal(u1.raw_content, "面白い");
const react = core.client.raw.prepare("SELECT * FROM reactions WHERE utterance_id = ?").get("ext:steam:r1") as any;
assert.equal(react.reaction_type, "positive");
assert.equal(react.intensity, 5);
console.log("ok external steam import");

// --- 2. 冪等性 (再 import は全 skip) ---
const r2 = importExternalUtterances(core, steam, { workspaceId: "knowledge", ingested });
assert.equal(r2.utterances, 0);
assert.equal(r2.skipped, 2);
const total = core.client.raw.prepare("SELECT COUNT(*) AS c FROM utterances").get() as { c: number };
assert.equal(total.c, 2, "重複投入されない");
console.log("ok idempotent re-import");

// --- 3. スレッド構造 (responds_to 解決) ---
const threaded: ExternalUtterance[] = [
  { source: "reddit", nativeId: "c2", gameSlug: "vs", threadKey: "t1", content: "返信", postedAt: 200, authorId: "U2", parentNativeId: "c1", sourceUrl: "u" },
  { source: "reddit", nativeId: "c1", gameSlug: "vs", threadKey: "t1", content: "親", postedAt: 100, authorId: "U1", sourceUrl: "u" },
];
const r3 = importExternalUtterances(core, threaded, { workspaceId: "knowledge", ingested });
assert.equal(r3.utterances, 2);
const child = core.client.raw.prepare("SELECT responds_to FROM utterances WHERE id = ?").get("ext:reddit:c2") as any;
assert.equal(child.responds_to, "ext:reddit:c1", "返信は親 utterance を指す");
console.log("ok thread responds_to");

ingested.close();
core.close();
console.log("crawler external tests: all passed");
