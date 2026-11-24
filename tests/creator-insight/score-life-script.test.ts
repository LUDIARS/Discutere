import assert from "node:assert/strict";
import { InputError } from "../../scripts/creator-insight/lib/questionnaire.js";
import { parseAnswers, scoreLifeScript } from "../../scripts/creator-insight/score-life-script.js";
import { exampleAnswers, expectedCard, questionnaire, uniformAnswers } from "./fixtures.js";

const q = questionnaire();

// 受入テスト: 架空の回答例を採点すると、特徴カード例の questionnaire.lifeScript と一致する。
assert.deepEqual({ lifeScript: scoreLifeScript(q, exampleAnswers(q)) }, expectedCard());

// 同じ入力からは同じ出力。
assert.deepEqual(scoreLifeScript(q, exampleAnswers(q)), scoreLifeScript(q, exampleAnswers(q)));

// 設問に無い ID・範囲外の値・版の違いはエラー。
const base = { questionnaire: q.id, revision: q.revision, answers: {}, forcedChoice: {} };
assert.throws(() => parseAnswers({ ...base, answers: { "X-1": 2 } }, q), InputError);
assert.throws(() => parseAnswers({ ...base, answers: { "D-BP-1": 5 } }, q), InputError);
assert.throws(() => parseAnswers({ ...base, answers: { "D-BP-1": 2.5 } }, q), InputError);
assert.throws(() => parseAnswers({ ...base, revision: "1999-01-01" }, q), InputError);
assert.throws(() => parseAnswers({ ...base, forcedChoice: { "FC-1": "z" } }, q), InputError);

// 回答が項目数の 75% に満たない尺度は未回答 (null)。4 問中 2 問だけ答えた尺度。
const partial = exampleAnswers(q);
delete partial.answers["D-HU-1"];
delete partial.answers["D-HU-2"];
const partialResult = scoreLifeScript(q, partial);
assert.equal(partialResult.drivers.scores.hurry_up, null);
assert.equal(partialResult.answered, 49);

// 平坦 (標準偏差 0.4 未満) なら優位を出さない。全問 3 は一方向ではない。
const flat = scoreLifeScript(q, uniformAnswers(q, 3));
assert.equal(flat.responseStyle.flat, true);
assert.equal(flat.responseStyle.oneSided, false);
assert.deepEqual(flat.drivers.dominant, []);
assert.deepEqual(flat.process.dominant, []);

// 全問 4 は一方向 (平均 3.5 以上)。逆転項目は 1 に変わるので逆転の矛盾が出る。
const high = scoreLifeScript(q, uniformAnswers(q, 4));
assert.equal(high.responseStyle.oneSided, true);
assert.ok(high.responseStyle.reverseConflicts.some((c) => c.item === "D-BP-4" && c.key === "be_perfect"));

// 基本的構え: 2.25〜2.75 は境界、それより上は肯定、下は否定。
assert.equal(scoreLifeScript(q, exampleAnswers(q)).position.selfBand, "border");
const lowSelf = exampleAnswers(q);
Object.assign(lowSelf.answers, { "P-S-1": 1, "P-S-2": 1, "P-S-3": 4, "P-S-4": 4 });
assert.equal(scoreLifeScript(q, lowSelf).position.selfBand, "negative");

// 逆転の矛盾がある優位の尺度は確度「低」。完全であれの逆転項目だけ食い違わせる。
const conflicted = exampleAnswers(q);
Object.assign(conflicted.answers, { "D-BP-1": 4, "D-BP-2": 4, "D-BP-3": 4, "D-BP-4": 4 });
const conflictedResult = scoreLifeScript(q, conflicted);
assert.ok(conflictedResult.responseStyle.reverseConflicts.some((c) => c.item === "D-BP-4"));
// 完全であれ = (4+4+4+1)/4 = 3.25、本人平均 2.65 より 0.6 高いので優位のまま、確度だけ「低」になる。
assert.ok(conflictedResult.drivers.dominant.includes("be_perfect"));
assert.equal(conflictedResult.drivers.confidence.be_perfect, "low");
