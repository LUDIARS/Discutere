/**
 * neco レビュー (2026-10-13、議論 #142) の対応テスト。
 * - 進行役の引用は元の発言のまま (LLM に言い換えさせない)・文の区切りで切る
 * - 反論の判定に「自分の陣営の主張と逆向き (self_contradiction)」を加える
 * - 「認める」の申告が中身と合っているかの判定 (障害は申告どおり)
 * - 定立の向き (論点に はい/いいえ) の判定と作り直しの指摘文
 */

import assert from "node:assert/strict";

const { quoteExcerpt, renderNomination, nominationFrameText } = await import("../../src/flow/dialectic/scheduler.js");
const { buildRuleJudgePrompt, parseRuleVerdict, RULE_FOUL_LABEL, judgeConcession, buildConcessionPrompt } = await import(
  "../../src/flow/dialectic/rule-check.js"
);
const { parseStanceAnswer, isReversed, buildStancePrompt, stanceFeedback, judgePositionStance } = await import(
  "../../src/flow/dialectic/position-stance.js"
);

const issue = { id: "i", sessionId: "s", title: "メカニクスの十分な面白さ", ordinal: 1, source: "paper-gate" as const, status: "open" as const };
const position = (stance: "pro" | "con", claim: string) => ({
  id: `p-${stance}`,
  issueId: "i",
  personaId: `u-${stance}`,
  stance,
  claim,
  qualifier: null,
  grounds: [{ id: `${stance}-G1`, text: "データ", state: "unchallenged" as const }],
  values: [],
});

// ── 引用 ──────────────────────────────────────────────────────────
{
  assert.equal(quoteExcerpt("短い発言。"), "短い発言。");
  const long = "V6から言えるのは編成が奥深いことまでです。".repeat(3) + "手持ちも敵も固定されたまま最適解探しが何年も尽きない、とは言えません。";
  const q = quoteExcerpt(long, 60);
  assert.ok(q.length <= 60 && q.endsWith("。"), "文の区切りで切る");
  assert.ok(long.startsWith(q), "元の文のまま (言い換えない)");
  assert.ok(quoteExcerpt("あ".repeat(200), 50).endsWith("…"), "区切りが無ければ上限で切って …");

  const target = { id: "t", personaName: "白石 詩織", text: "手持ちも敵も固定されたまま最適解探しが何年も尽きない、とは言えません。" };
  const rewriting = { invoke: async () => ({ ok: true as const, text: "白石さんの指摘に応えてください。" }) };
  const line = await renderNomination({
    decision: { kind: "respond", personaId: "u", targetUtteranceId: "t", allowedActs: [], reason: "" },
    personaName: "神谷 蓮",
    target,
    llm: rewriting as never,
  });
  assert.ok(line.endsWith(`> ${target.text}`), "引用は LLM を通さず元の発言をそのまま付ける");
  assert.ok(!nominationFrameText({ kind: "respond", personaId: "u", targetUtteranceId: "t", allowedActs: [], reason: "" }, "神谷", target).includes("言えません"),
    "LLM に渡す枠の文には引用を含めない");
  console.log("  [ok] 進行役の引用は元の発言のまま");
}

// ── 立場の取り違え ────────────────────────────────────────────────
{
  const prompt = buildRuleJudgePrompt({
    issue,
    position: position("pro", "メカニクスは十分面白い"),
    ownPosition: position("con", "更新は本質ではない"),
    ground: null,
    attack: "warrant",
    rebutText: "長期継続は更新ありきで成立している",
    rebutData: "7年は更新が続いた7年",
    rebutWarrant: "更新が無い状態の継続は示せない",
  });
  assert.match(prompt, /反論した側の陣営の主張[^\n]*\n主張: 更新は本質ではない/);
  assert.match(prompt, /- self_contradiction:/);
  assert.equal(parseRuleVerdict("self_contradiction"), "self_contradiction");
  assert.ok(RULE_FOUL_LABEL.self_contradiction.includes("自分の陣営の主張と逆"));

  const noOwn = buildRuleJudgePrompt({
    issue,
    position: position("pro", "x"),
    ground: null,
    attack: "data",
    rebutText: "r",
    rebutData: "d",
    rebutWarrant: "w",
  });
  assert.ok(!noOwn.includes("self_contradiction"), "陣営の主張が無ければ判定しない");
  console.log("  [ok] 反論の立場の取り違えを判定する");
}

// ── 「認める」の検証 ──────────────────────────────────────────────
{
  const llm = (text: string) => ({ invoke: async () => ({ ok: true as const, text }) }) as never;
  assert.equal(await judgeConcession({ targetText: "t", replyText: "r", llm: llm("rebut") }), "rebut");
  assert.equal(await judgeConcession({ targetText: "t", replyText: "r", llm: llm("concede") }), "concede");
  const warns: string[] = [];
  const failing = { invoke: async () => ({ ok: false as const, error: "down" }) } as never;
  assert.equal(await judgeConcession({ targetText: "t", replyText: "r", llm: failing, warn: (m) => warns.push(m) }), "concede");
  assert.equal(await judgeConcession({ targetText: "t", replyText: "r", llm: llm("???"), warn: (m) => warns.push(m) }), "concede");
  assert.equal(warns.length, 2, "障害・不明は申告どおり (warn 明示)");
  assert.match(buildConcessionPrompt("相手", "返答"), /concede: .*\n- rebut:/);
  console.log("  [ok] 認めるの検証");
}

// ── 定立の向き ────────────────────────────────────────────────────
{
  assert.equal(parseStanceAnswer("yes"), "yes");
  assert.equal(parseStanceAnswer("ラベル: no"), "no");
  assert.equal(parseStanceAnswer("わからない"), "unclear");
  assert.equal(isReversed("con", "yes"), true, "反対なのに肯定は逆向き");
  assert.equal(isReversed("pro", "no"), true);
  assert.equal(isReversed("pro", "yes"), false);
  assert.equal(isReversed("con", "unclear"), false, "不明は作り直さない");
  assert.match(buildStancePrompt(issue, "メカニクスだけで十分", null), /は成り立つか \/ 十分か/);
  assert.match(stanceFeedback("con", "メカニクスだけで十分"), /【反対】の立場と逆向き.*「いいえ」/);
  const failing = { invoke: async () => ({ ok: false as const, error: "down" }) } as never;
  assert.equal(await judgePositionStance({ issue, claim: "x", qualifier: null, llm: failing }), "unclear");
  console.log("  [ok] 定立の向きの判定");
}

console.log("dialectic-review-fixes tests: all passed");
