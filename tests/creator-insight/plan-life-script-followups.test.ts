import assert from "node:assert/strict";
import { InputError, readJsonFile } from "../../scripts/creator-insight/lib/questionnaire.js";
import { parseBank, parseResult, planFollowups } from "../../scripts/creator-insight/plan-life-script-followups.js";
import { scoreLifeScript } from "../../scripts/creator-insight/score-life-script.js";
import { exampleAnswers, followupsPath, questionnaire, uniformAnswers } from "./fixtures.js";

const q = questionnaire();
const bank = parseBank(readJsonFile(followupsPath), q);

// 回答例: 優位は 完全であれ・強くあれ / 〜までは、強くあれは場面の選択で 1 回だけ、構えは境界。
const plan = planFollowups(scoreLifeScript(q, exampleAnswers(q)), bank, q);
assert.deepEqual(
  plan.topics.map((t) => `${t.kind}:${t.keys.join("+")}`),
  [
    "forced_choice_mismatch:be_strong",
    "dominant_driver:be_perfect",
    "dominant_driver:be_strong",
    "dominant_process:until",
    "correspondence_mismatch:be_strong+never",
  ],
);
assert.ok(plan.topics.length <= bank.limits.maxTopics);
assert.equal(plan.maxDeepeningPerTopic, 1);
assert.deepEqual(plan.topics.map((t) => t.n), [1, 2, 3, 4, 5]);
// 根拠は採点結果の値を指す。最初の問いは追加質問 JSON の文例 (テンプレートの穴が残らない)。
assert.ok(plan.topics[0]!.basis.includes("drivers.forcedChoice.be_strong=1"));
assert.equal(plan.topics[1]!.ask, bank.drivers.be_perfect!.ask);
assert.equal(plan.topics[1]!.strength, "丁寧さ");
assert.ok(plan.topics.every((t) => !/\{\w+\}/.test(t.ask)));
// 問いに専門用語を出さない (neco 2026-11-25)。分析の呼び名は plainLabels に置き換わる。
const jargon = [...Object.values(q.labels), "脚本", "ドライバー", "禁止令", "構え"];
for (const t of plan.topics) for (const word of jargon) assert.ok(!t.ask.includes(word), `jargon "${word}" in: ${t.ask}`);
assert.ok(plan.topics[0]!.ask.includes(bank.plainLabels.be_strong!));
// 文例の全部も専門用語なし。
const bankAsks = [
  ...Object.values(bank.drivers).map((d) => d.ask), ...Object.values(bank.process).map((p) => p.ask),
  ...Object.values(bank.position), ...Object.values(bank.templates),
];
for (const ask of bankAsks) for (const word of jargon) assert.ok(!ask.includes(word), `jargon "${word}" in bank: ${ask}`);

// 同じ入力からは同じ出力。
assert.deepEqual(planFollowups(scoreLifeScript(q, exampleAnswers(q)), bank, q), plan);

// 平坦な回答: 答え方の偏りを先に聞き、優位の話題は出ない。
const flat = planFollowups(scoreLifeScript(q, uniformAnswers(q, 3)), bank, q);
assert.equal(flat.topics[0]!.kind, "response_style");
assert.ok(!flat.topics.some((t) => t.kind === "dominant_driver" || t.kind === "dominant_process"));

// 話題が 1 つも無いときは、開いた問いを 1 つだけ出す。
const empty = scoreLifeScript(q, { questionnaire: q.id, revision: q.revision, answers: {}, forcedChoice: {} });
const open = planFollowups(empty, bank, q);
assert.deepEqual(open.topics.map((t) => t.kind), ["open"]);

// 採点結果は score-life-script の出力 ({ lifeScript }) でも中身だけでも受け付け、別の設問の結果は拒む。
assert.deepEqual(parseResult({ lifeScript: empty }, q), empty);
assert.throws(() => parseResult({ ...empty, questionnaire: "other" }, q), InputError);
assert.throws(() => parseBank({ questionnaire: "other" }, q), InputError);
