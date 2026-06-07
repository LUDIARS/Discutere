import assert from "node:assert/strict";
import Database from "better-sqlite3";

import { buildDataSourceSnapshot } from "../../src/visualize/data-sources.js";

const db = new Database(":memory:");
db.exec(`
  CREATE TABLE sessions (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, scene TEXT);
  CREATE TABLE utterances (id TEXT PRIMARY KEY, workspace_id TEXT, session_id TEXT, raw_content TEXT);
`);

const ws = "knowledge";
const addSession = (id: string, title: string | null, scene: string | null) =>
  db.prepare("INSERT INTO sessions VALUES (?,?,?,?)").run(id, ws, title, scene);
const addUtt = (id: string, sid: string) =>
  db.prepare("INSERT INTO utterances VALUES (?,?,?,?)").run(id, ws, sid, "x");

// website: ワンダ 2 記事 (2 utt)
addSession("s1", "website:shadow-of-the-colossus", null);
addUtt("u1", "s1");
addSession("s2", "website:shadow-of-the-colossus", null);
addUtt("u2", "s2");
// youtube: ワンダ 1 動画 (3 utt)
addSession("s3", "youtube:shadow-of-the-colossus", null);
addUtt("u3", "s3");
addUtt("u4", "s3");
addUtt("u5", "s3");
// steam: 別ゲーム (1 utt)
addSession("s4", "steam:vampire-survivors", null);
addUtt("u6", "s4");
// 内部生成: 自走議論 (gap uuid は game に数えない)
addSession("s5", "discussion-of-gap:3460fdc0-9a31-4f90-bcd6-979fb9073157", null);
addUtt("u7", "s5");
// title 無し + discord scene
addSession("s6", null, "discord:guild/thread");
addUtt("u8", "s6");
// 別 workspace は集計対象外
db.prepare("INSERT INTO sessions VALUES (?,?,?,?)").run("s9", "other", "steam:x", null);
db.prepare("INSERT INTO utterances VALUES (?,?,?,?)").run("u9", "other", "s9", "x");

const snap = buildDataSourceSnapshot(db, ws, 1000);

// ソース種別カウント: website/youtube/steam=import, discussion-of-gap/discord-session=internal
assert.equal(snap.totals.sources, 5, "source kinds");
assert.equal(snap.totals.importSources, 3, "import sources = website/youtube/steam");
assert.equal(snap.totals.internalSources, 2, "internal = gap/discord");
assert.equal(snap.totals.utterances, 8, "total utterances (別ws除外)");
assert.equal(snap.totals.importUtterances, 6, "import utterances = 2+3+1");
assert.equal(snap.totals.games, 2, "games = SOTC + vampire-survivors (gap uuid 除外)");

// 並びは utterances 降順 → youtube(3) が website(2) より先
const website = snap.sources.find((s) => s.source === "website");
assert.ok(website, "website present");
assert.equal(website.utterances, 2);
assert.equal(website.sessions, 2);
assert.deepEqual(website.games, ["shadow-of-the-colossus"]);
assert.equal(website.kind, "import");

// ゲーム別内訳: SOTC は website+youtube の 2 ソース、発話 5
const sotc = snap.byGame.find((g) => g.slug === "shadow-of-the-colossus");
assert.ok(sotc, "SOTC breakdown present");
assert.equal(sotc.utterances, 5);
assert.equal(sotc.sources.length, 2);
assert.equal(sotc.sources[0].source, "youtube", "発話降順で youtube が先頭");

console.log("data-sources test: ok");
