import assert from "node:assert/strict";
import Database from "better-sqlite3";

import {
  listFlowConclusions,
  getFlowConclusionDetail,
  isFlowConclusionId,
  flowSessionIdFromGapId,
  FLOW_CONCLUSION_PREFIX,
} from "../../src/visualize/flow-conclusions.js";

const db = new Database(":memory:");
db.exec(`
  CREATE TABLE discussion_paper (
    id TEXT PRIMARY KEY,
    theme TEXT NOT NULL,
    tags_json TEXT NOT NULL DEFAULT '[]',
    mechanics_json TEXT NOT NULL DEFAULT '[]',
    supplement TEXT NOT NULL DEFAULT ''
  );
  CREATE TABLE flow_conclusion (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    paper_id TEXT NOT NULL,
    summary TEXT NOT NULL DEFAULT '',
    aufhebung_json TEXT NOT NULL DEFAULT '[]',
    top_utterance_ids_json TEXT NOT NULL DEFAULT '[]',
    concluded INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL
  );
  CREATE TABLE flow_utterance (
    id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL,
    paper_id TEXT NOT NULL,
    round INTEGER NOT NULL DEFAULT 1,
    turn INTEGER NOT NULL DEFAULT 1,
    persona_id TEXT NOT NULL DEFAULT '',
    persona_name TEXT NOT NULL,
    role TEXT NOT NULL DEFAULT 'opinion',
    stance TEXT NOT NULL DEFAULT 'neutral',
    possession_name TEXT,
    text TEXT NOT NULL,
    is_error INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL
  );
  CREATE TABLE vote (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    round INTEGER NOT NULL,
    voter_index INTEGER NOT NULL,
    chosen_utterance_id TEXT,
    created_at INTEGER NOT NULL
  );
`);

// 議論 1: 収束済み
db.prepare(
  "INSERT INTO discussion_paper (id, theme, tags_json, mechanics_json, supplement) VALUES (?,?,?,?,?)"
).run(
  "p1",
  "ローグライトの周回設計",
  JSON.stringify(["開発", "内部"]),
  JSON.stringify([
    { name: "ビルド選択", description: "周回ごとに構成を変える", intended_affect: "発見" },
    { name: "難度上昇", description: "周回で敵が強くなる" },
  ]),
  "社外秘の数値には触れない。"
);
db.prepare(
  "INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at) VALUES (?,?,?,?,?,?,?)"
).run(
  "sess-1",
  "p1",
  "【収束】周回はビルド多様性で飽きを防ぐ",
  JSON.stringify(["難度と報酬の対応を保つ", "メタ進行は控えめに"]),
  JSON.stringify(["u2"]),
  1,
  2000
);
// 議論 2: 結論なし (concluded=0) — paper も欠落 (LEFT JOIN フォールバック検証)
db.prepare(
  "INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at) VALUES (?,?,?,?,?,?,?)"
).run("sess-2", "p-missing", "(結論なし)", "[]", "[]", 0, 3000);

// 発話 (議論 1)
for (const [id, name, stance, text, err, t] of [
  ["u1", "陽介", "pro", "周回前提なら飽きが課題", 0, 100],
  ["u2", "ミナ", "con", "ビルド分岐で多様性を出せる", 0, 110],
  ["uErr", "進行役", "neutral", "(エラー)", 1, 120],
] as const) {
  db.prepare(
    "INSERT INTO flow_utterance (id, session_id, paper_id, persona_name, stance, possession_name, text, is_error, created_at) VALUES (?,?,?,?,?,?,?,?,?)"
  ).run(id, "sess-1", "p1", name, stance, null, text, err, t);
}
// 投票: u2 に 2 票
db.prepare("INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES (?,?,?,?,?)").run("sess-1", 1, 0, "u2", 130);
db.prepare("INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES (?,?,?,?,?)").run("sess-1", 1, 1, "u2", 131);

// --- id ヘルパ ---
assert.equal(isFlowConclusionId("flow:sess-1"), true);
assert.equal(isFlowConclusionId("gap-123"), false);
assert.equal(flowSessionIdFromGapId(`${FLOW_CONCLUSION_PREFIX}sess-1`), "sess-1");

// --- 一覧 ---
const list = listFlowConclusions(db, 100);
assert.equal(list.length, 2);
// 新しい順 (created_at desc): sess-2 が先
assert.equal(list[0].sessionId, "sess-2");
assert.equal(list[0].kind, "flow");
assert.equal(list[0].gapId, "flow:sess-2");
assert.equal(list[0].conclusion, null); // concluded=0 → null
assert.equal(list[0].title, "(無題の議論)"); // paper 欠落フォールバック

const c1 = list[1];
assert.equal(c1.sessionId, "sess-1");
assert.equal(c1.title, "ローグライトの周回設計");
assert.equal(c1.conclusion, "周回はビルド多様性で飽きを防ぐ"); // 【収束】除去
assert.equal(c1.utteranceCount, 3);
assert.equal(c1.aufhebungCount, 2);

// --- 詳細 ---
const detail = getFlowConclusionDetail(db, "sess-1");
assert.ok(detail);
assert.equal(detail.title, "ローグライトの周回設計");
assert.equal(detail.aufhebungen.length, 2);
// transcript は is_error を除外 → 2 件
assert.equal(detail.transcript.length, 2);
assert.equal(detail.transcript[0].speaker, "陽介 (賛成派)");
assert.equal(detail.transcript[0].source, null);
// topOpinions = winner (u2) のみ、 score = 得票数 2
assert.equal(detail.topOpinions.length, 1);
assert.equal(detail.topOpinions[0].score, 2);
assert.equal(detail.topOpinions[0].speaker, "ミナ (反対派)");

// ディスカッションペーパー (議題ブリーフ) が JOIN で組み立つ
assert.ok(detail.paper);
assert.equal(detail.paper.theme, "ローグライトの周回設計");
assert.deepEqual(detail.paper.tags, ["開発", "内部"]);
assert.equal(detail.paper.supplement, "社外秘の数値には触れない。");
assert.equal(detail.paper.mechanics.length, 2);
assert.equal(detail.paper.mechanics[0].name, "ビルド選択");
assert.equal(detail.paper.mechanics[0].intendedAffect, "発見");
assert.equal(detail.paper.mechanics[1].intendedAffect, undefined);

// paper が JOIN できない (paper_id 不一致) 議論は paper=null
const detail2 = getFlowConclusionDetail(db, "sess-2");
assert.ok(detail2);
assert.equal(detail2.paper, null);

// 存在しない session → null
assert.equal(getFlowConclusionDetail(db, "nope"), null);

db.close();

console.log("flow-conclusions.test.ts: all passed");
