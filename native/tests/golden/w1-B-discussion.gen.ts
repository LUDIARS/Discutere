// discussion (Node 実装) の編成・進行を決定論的 rng で記録する。
// 実行: npx tsx native/tests/golden/w1-B-discussion.gen.ts
// 出力: native/tests/fixtures/discussion/cases.json
import fs from "node:fs";
import path from "node:path";

import { formParty, swapHalf, partySize, pickWeightedModel, type PartyConfig } from "../../../src/discussion/composition.js";
import { runDiscussion } from "../../../src/discussion/director.js";

const seq = [0.1, 0.5, 0.9, 0.2, 0.7, 0.3, 0.6, 0.4, 0.8, 0.05, 0.15, 0.25, 0.99, 0.0, 0.45];
const mkRng = () => { let i = 0; return () => seq[i++ % seq.length]; };

const configs: PartyConfig[] = [
  { expectedUtterances: 20, keymanCount: 2, facilitatorModel: "opus", keymanModel: "opus", opinionModels: [{ model: "sonnet", weight: 6 }, { model: "haiku", weight: 4 }], minTotal: 4 },
  { expectedUtterances: 3, keymanCount: 2, facilitatorModel: "opus", keymanModel: "opus", opinionModels: [{ model: "sonnet", weight: 6 }, { model: "haiku", weight: 4 }], minTotal: 4 },
  { expectedUtterances: 100, keymanCount: 1, facilitatorModel: "f", keymanModel: "k", opinionModels: [{ model: "a", weight: 0 }, { model: "b", weight: -1 }], minTotal: 2 },
];
const parties = configs.map((cfg) => {
  const rng = mkRng();
  const party = formParty(cfg, rng);
  const swapped = swapHalf(party, cfg, rng);
  return { cfg, party, swapped };
});

async function main() {
  const runs = [];
  for (const [cfgIndex, maxRounds, continueTimes, failEvery] of [[1, 3, 1, 0], [1, 2, 5, 3], [0, 1, 0, 0]] as const) {
    const rng = mkRng();
    const calls: unknown[] = [];
    const posted: unknown[] = [];
    const logs: string[] = [];
    let n = 0;
    let asks = 0;
    let summary: unknown = null;
    const result = await runDiscussion({ topic: "テスト議題\n二行目", partyConfig: configs[cfgIndex] }, {
      llm: { invoke: async (a) => { calls.push(a); n++; if (failEvery && n % failEvery === 0) return n % 2 ? { ok: false, error: "boom" } : { ok: true, text: "   " }; return { ok: true, text: ` 発言${n} ` }; } },
      postUtterance: async (m, text) => { posted.push([m.slotId, m.personaName, text]); },
      askContinue: async (ctx) => { asks++; posted.push(["ask", ctx]); return asks <= continueTimes; },
      postSummary: async (m, text) => { summary = [m.slotId, text]; },
      log: (m) => logs.push(m),
      rng,
      maxRounds,
    });
    runs.push({ cfgIndex, maxRounds, continueTimes, failEvery, result, calls, posted, logs, summary });
  }
  const sizes = [0, 1, 8, 12, 13, 20, 100].map((e) => ({ e, size: partySize(e) }));
  const picks = [0, 0.3, 0.59, 0.6, 0.99].map((r) => ({ r, model: pickWeightedModel(configs[0].opinionModels, () => r) }));
  const out = { configs, parties, runs, sizes, picks };
  const target = path.resolve("native/tests/fixtures/discussion/cases.json");
  fs.writeFileSync(target, JSON.stringify(out, null, 2) + "\n");
  console.log(`wrote ${target}`);
}
void main();
