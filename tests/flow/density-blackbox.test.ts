import assert from "node:assert/strict";

import { makeMemoryBlackBox } from "@ludiars/blackbox";
import { makeBlackboxDensityEvaluator, densityFeatures, DOMAIN_DENSITY } from "../../src/flow/density-blackbox.js";
import type { EvaluateArgs, InformationEvaluation } from "../../src/flow/information-gate.js";
import type { ContextVoice } from "../../src/flow/discussion-paper.js";
import type { LLMClient } from "../../src/persona-engine/llm/client.js";

const dummyLlm: LLMClient = {
  async invoke() {
    return { ok: false, error: "unused" };
  },
};

function voicesOf(n: number, source = "niconico"): ContextVoice[] {
  return Array.from({ length: n }, () => ({ content: "1234567890", source }) as ContextVoice);
}

function argsOf(voices: ContextVoice[]): EvaluateArgs {
  return { theme: "テーマ", tags: [], voices, llm: dummyLlm };
}

/** rich + 閾値ルール候補を返す fake evaluateInformationDensity。 */
function fakeEvaluate(counter: { calls: number }): (args: EvaluateArgs) => Promise<InformationEvaluation> {
  return async () => {
    counter.calls += 1;
    return {
      density: "rich",
      covered: ["肯定", "否定"],
      gaps: [],
      proposedRule: {
        description: "8件以上は rich",
        when: { op: "cmp", feature: "voiceCount", cmp: ">=", value: 8 },
      },
    };
  };
}

// --- densityFeatures: フラット特徴量 ---
{
  const f = densityFeatures(voicesOf(3).concat(voicesOf(2, "youtube")), ["機密"] as never);
  assert.equal(f.voiceCount, 5);
  assert.equal(f.sourceKinds, 2);
  assert.equal(f.avgLen, 10);
  assert.equal(f.tagCount, 1);
}

// --- LLM 経路: 完全な評価 (covered/gaps) を透過し、candidate が蓄積される ---
{
  const bb = makeMemoryBlackBox();
  const counter = { calls: 0 };
  const evaluate = makeBlackboxDensityEvaluator(bb, fakeEvaluate(counter));
  const ev = await evaluate(argsOf(voicesOf(10)));
  assert.equal(ev.density, "rich");
  assert.deepEqual(ev.covered, ["肯定", "否定"]);
  assert.equal(counter.calls, 1);
  const rules = bb.rules.listByDomain(DOMAIN_DENSITY);
  assert.equal(rules.length, 1);
  assert.equal(rules[0].state, "candidate");
}

// --- 影評価で trial へ昇格し、以後 LLM を呼ばずルールで即決 (gaps 空 = theme fallback) ---
{
  const bb = makeMemoryBlackBox();
  const counter = { calls: 0 };
  const evaluate = makeBlackboxDensityEvaluator(bb, fakeEvaluate(counter));
  for (let i = 0; i < 4; i++) await evaluate(argsOf(voicesOf(10)));
  assert.equal(bb.rules.listByDomain(DOMAIN_DENSITY)[0].state, "trial");
  assert.equal(counter.calls, 4);
  const ev = await evaluate(argsOf(voicesOf(12)));
  assert.equal(counter.calls, 4); // LLM 不要
  assert.equal(ev.density, "rich");
  assert.deepEqual(ev.gaps, []);
}

// --- 条件不一致 (声が少ない) ならルールは発火せず LLM に戻る ---
{
  const bb = makeMemoryBlackBox();
  const counter = { calls: 0 };
  const evaluate = makeBlackboxDensityEvaluator(bb, fakeEvaluate(counter));
  for (let i = 0; i < 4; i++) await evaluate(argsOf(voicesOf(10))); // trial 化
  await evaluate(argsOf(voicesOf(2))); // voiceCount=2 は when 不一致
  assert.equal(counter.calls, 5);
}

// --- proposedRule が不正でも評価は落ちない (候補登録だけスキップ) ---
{
  const bb = makeMemoryBlackBox();
  const evaluate = makeBlackboxDensityEvaluator(bb, async () => ({
    density: "moderate",
    covered: [],
    gaps: [],
    proposedRule: { when: { op: "bogus" } },
  }));
  const ev = await evaluate(argsOf(voicesOf(4)));
  assert.equal(ev.density, "moderate");
  assert.equal(bb.rules.listByDomain(DOMAIN_DENSITY).length, 0);
}

console.log("density-blackbox.test: all assertions passed");
