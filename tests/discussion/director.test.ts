/**
 * DiscussionDirector の offline テスト (mock LLM / inject deps)。
 * 予算到達 → 続行(1回, 半数入替) → 予算到達 → 停止 の流れを検証。
 */
import assert from "node:assert/strict";

import { runDiscussion, type DirectorDeps } from "../../src/discussion/director.js";
import type { PartyConfig } from "../../src/discussion/composition.js";

const partyConfig: PartyConfig = {
  expectedUtterances: 3, // テスト短縮 (本番既定は 20)
  keymanCount: 2,
  facilitatorModel: "opus",
  keymanModel: "opus",
  opinionModels: [
    { model: "sonnet", weight: 6 },
    { model: "haiku", weight: 4 },
  ],
  minTotal: 4,
};

let n = 0;
const posted: Array<{ name: string; text: string }> = [];
let askCalls = 0;
let summary: string | null = null;
const swapWitnessRound1: string[] = [];
const swapWitnessRound2: string[] = [];

const deps: DirectorDeps = {
  llm: {
    invoke: async ({ model }) => ({ ok: true, text: `発言${++n} (model=${model})` }),
  },
  postUtterance: async (m, text) => {
    posted.push({ name: m.personaName, text });
    (askCalls === 0 ? swapWitnessRound1 : swapWitnessRound2).push(m.personaName);
  },
  askContinue: async () => {
    askCalls++;
    return askCalls === 1; // 1 回目だけ続行、2 回目は停止
  },
  postSummary: async (_m, text) => {
    summary = text;
  },
  log: () => {},
  rng: (() => {
    let i = 0;
    const seq = [0.1, 0.5, 0.9, 0.2, 0.7, 0.3, 0.6, 0.4, 0.8, 0.05, 0.15, 0.25];
    return () => seq[i++ % seq.length];
  })(),
  maxRounds: 3,
};

const result = await runDiscussion({ topic: "テスト議題", partyConfig }, deps);

assert.equal(result.stoppedBy, "user-stop", "2 回目の問いで停止");
assert.equal(result.rounds, 2, "2 ラウンド進行");
assert.equal(askCalls, 2, "続行/停止の問いは 2 回");
assert.equal(result.utterances.length, 6, "3 発話 × 2 ラウンド = 6");
console.log("ok rounds / budget gate (6 発話, 2 ラウンド, 停止)");

// 半数入替の確認: round1 と round2 のメンバー集合が一部入れ替わっている
const r1 = new Set(swapWitnessRound1);
const r2 = new Set(swapWitnessRound2);
const newcomers = [...r2].filter((nm) => !r1.has(nm));
assert.ok(newcomers.length >= 1, `round2 に新メンバーが入る (newcomers=${newcomers.join(",")})`);
console.log(`ok swapHalf reflected (newcomers: ${newcomers.join(", ")})`);

assert.ok(summary && summary.length > 0, "司会まとめが出力される");
console.log("ok facilitator summary");

console.log("director.test.ts: all passed");
