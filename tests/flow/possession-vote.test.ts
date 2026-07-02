/**
 * 憑依 (item4) + 投票通知 (item3) の director 結線テスト。
 *  - プールに theme 近傍ペルソナを 1 体入れると opinion 枠が「憑依」する
 *  - 憑依発話は personaName=casual 名を保ち、possessionName=データ由来ペルソナ名を持つ
 *  - ラウンド投票後に onVote が candidates 付きで呼ばれる
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";

const TMP_DIR = path.resolve(".tmp/flow-possession-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });
process.env.DATABASE_PATH = path.join(TMP_DIR, "possession.db");

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();

const { _resetConfig } = await import("../../src/config.js");
_resetConfig();

const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");
const { insertPoolPersona } = await import("../../src/flow/persona-pool.js");
const { textToVector } = await import("../../src/flow/sentiment-vector.js");
const { runDiscussionFlow } = await import("../../src/flow/director.js");

const THEME = "ローグライトの中毒性はどこから来るか";

// theme 近傍 (= 同一ベクトル) の採用ペルソナをプールに 1 体入れる → 憑依対象になる。
insertPoolPersona({
  id: "pool-possess-1",
  name: "論者#test01",
  role: "opinion",
  speechStyle: "",
  traits: [],
  affectVector: textToVector(THEME),
  origin: "adopted",
  parentIds: [],
  learningSource: "youtube",
  sourceSpeakerId: "ext:youtube:tester",
  typicality: 0.95,
  polarityBias: 0.6,
  affectDispersion: 0.2,
});

const utterances: Array<{ personaName: string; possessionName?: string; role: string }> = [];
const voteEvents: Array<{ round: number; candidates: Array<{ id: string; personaName: string }> }> = [];

// シャッフル輪番 (respec 02) で facilitator を除く全員が発言するよう turnsPerRound=人数 (3) にする
// → opinion 枠 (憑依席) が必ず 1 回発言する。rng 固定で順序は決定的。
// 全呼び出しで同じ平文を返す mock (responders 空 → fallback)。発話は非空 = 投票候補になる。
const result = await runDiscussionFlow(THEME, [], {
  llm: new MockLLMClient([], "なるほど、その視点は面白いと思う。"),
  rng: () => 0.3,
  rounds: 1,
  turnsPerRound: 3,
  onUtterance: (u) => {
    utterances.push({ personaName: u.personaName, possessionName: u.possessionName, role: u.role });
  },
  onVote: (e) => {
    voteEvents.push({ round: e.round, candidates: e.candidates });
  },
});

assert.ok(result.sessionId, "session 生成");

// 憑依した発話が存在する: possessionName=プール名、personaName!=プール名 (casual 名保持)。
const possessed = utterances.filter((u) => u.possessionName);
assert.ok(possessed.length > 0, "憑依発話が 1 件以上ある");
for (const u of possessed) {
  assert.equal(u.possessionName, "論者#test01", "possessionName = データ由来ペルソナ名");
  assert.notEqual(u.personaName, "論者#test01", "personaName は casual 名 (プール名で上書きしない)");
}
console.log(`  [ok] 憑依: ${possessed.length} 発話が possessionName を持つ (casual 名保持)`);

// onVote がラウンドごとに candidates 付きで呼ばれる。
assert.ok(voteEvents.length >= 1, "onVote が 1 回以上呼ばれる");
assert.ok(voteEvents[0].candidates.length > 0, "投票 candidates が空でない");
console.log(`  [ok] onVote: ${voteEvents.length} ラウンド分、candidates あり`);

console.log("possession-vote tests: all passed");
