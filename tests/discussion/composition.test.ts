/**
 * パーティ編成 (composition.ts) の単体テスト。決定論的 rng で検証。
 */
import assert from "node:assert/strict";

import {
  partySize,
  pickWeightedModel,
  formParty,
  swapHalf,
  decideStance,
  type PartyConfig,
} from "../../src/discussion/composition.js";

const CFG: PartyConfig = {
  expectedUtterances: 20,
  keymanCount: 2,
  facilitatorModel: "claude-opus-4-8",
  keymanModel: "claude-opus-4-8",
  opinionModels: [
    { model: "claude-sonnet-5", weight: 6 },
    { model: "claude-haiku-4-5-20251001", weight: 4 },
  ],
  minTotal: 4,
};

// 連番 rng (再現可能)
function seqRng(values: number[]): () => number {
  let i = 0;
  return () => values[i++ % values.length];
}

// 1. partySize
assert.equal(partySize(20), 6, "20 → 6");
assert.equal(partySize(12), 4, "12 → ceil(3)+1=4");
assert.equal(partySize(8), 4, "8 → max(4,3)=4");
assert.equal(partySize(100), 26, "100 → 26");
console.log("ok partySize");

// 2. pickWeightedModel: rng=0 → 先頭、rng≈1 → 末尾
assert.equal(pickWeightedModel(CFG.opinionModels, () => 0), "claude-sonnet-5");
assert.equal(pickWeightedModel(CFG.opinionModels, () => 0.99), "claude-haiku-4-5-20251001");
assert.equal(pickWeightedModel(CFG.opinionModels, () => 0.5), "claude-sonnet-5", "0.5*10=5 < 6 → sonnet");
console.log("ok pickWeightedModel");

// 3. formParty: expected=20 → 6 名 (司会1 + キーマン2 + 意見3)
const party = formParty(CFG, seqRng([0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.05]));
assert.equal(party.length, 6, "6 名");
assert.equal(party.filter((m) => m.tier === "facilitator").length, 1);
assert.equal(party.filter((m) => m.tier === "keyman").length, 2);
assert.equal(party.filter((m) => m.tier === "opinion").length, 3);
assert.ok(party.filter((m) => m.tier !== "opinion").every((m) => m.model === "claude-opus-4-8"), "司会/キーマンは Opus");
assert.ok(
  party.filter((m) => m.tier === "opinion").every((m) => /sonnet|haiku/.test(m.model)),
  "意見は Sonnet/Haiku"
);
const names = party.map((m) => m.personaName);
assert.equal(new Set(names).size, names.length, "名前は重複しない");
console.log("ok formParty (6 名, モデル割当, 名前一意)");

// 4. swapHalf: 司会不変、swapCount=floor(5/2)=2、キーマンは Opus 維持
const swapped = swapHalf(party, CFG, seqRng([0.9, 0.1, 0.5, 0.3, 0.7, 0.2, 0.4]));
const facBefore = party.find((m) => m.tier === "facilitator")!;
const facAfter = swapped.find((m) => m.tier === "facilitator")!;
assert.deepEqual(facAfter, facBefore, "司会は不変");
const changed = swapped.filter((m, i) => m.personaName !== party[i].personaName);
assert.equal(changed.length, 2, "半数 (2 名) 入替");
assert.ok(
  swapped.filter((m) => m.tier === "keyman").every((m) => m.model === "claude-opus-4-8"),
  "入替後もキーマンは Opus"
);
console.log("ok swapHalf (司会固定 / 2 名入替 / キーマン Opus 維持)");

// 5. decideStance
const fac = party.find((m) => m.tier === "facilitator")!;
const key = party.find((m) => m.tier === "keyman")!;
assert.equal(decideStance(fac, () => 0.1), "neutral", "司会は neutral");
assert.equal(decideStance(key, () => 0.1), "pro", "rng<0.5 → pro");
assert.equal(decideStance(key, () => 0.9), "con", "rng>=0.5 → con");
console.log("ok decideStance");

console.log("composition.test.ts: all passed");
