/**
 * 論点の結論の 3 区分 (dialectic.md §4.5) テスト。
 * - 論破 (refutation あり) / 止揚 (synthesized の Tension あり) / 結論なし (それ以外)
 * - 結論なしの理由 (折衷・批准不成立・事実照会・旧合意)
 * - 決着表 (settlementToRoundSummary) に区分ラベルが載る
 */

import assert from "node:assert/strict";

const { issueOutcome, noConclusionReason, renderOutcomeLine, OUTCOME_LABEL } = await import(
  "../../src/flow/dialectic/outcome.js"
);
const { settlementToRoundSummary } = await import("../../src/flow/dialectic/conclusion.js");

type Status = "synthesized" | "compromised" | "agreed_disagree" | "resolved" | "unresolved_fact" | "agreed";

function tension(status: Status, note: string | null = null) {
  return { id: `t-${status}`, issueId: "i1", positionA: "a", positionB: "b", type: "values" as const, status, resolutionNote: note };
}

function synthesis(tensionId: string, text: string) {
  return {
    id: `s-${tensionId}`,
    tensionId,
    kind: "aufheben",
    text,
    preserves: [],
    negates: [],
    elevates: "",
    status: "ratified",
    revision: 0,
    ratification: [],
  } as never;
}

const issue = { id: "i1", sessionId: "s1", title: "天井を下げるか", ordinal: 1, source: "llm" as const, status: "concluded" as const };

// ── 区分 ──────────────────────────────────────────────────────────
{
  assert.deepEqual(Object.values(OUTCOME_LABEL), ["止揚", "論破", "結論なし"]);
  assert.equal(issueOutcome({ refutation: "反対側の根拠がすべて崩れた", tensions: [] }), "refuted");
  assert.equal(issueOutcome({ refutation: null, tensions: [tension("synthesized")] }), "aufheben");
  for (const status of ["compromised", "agreed_disagree", "resolved", "unresolved_fact", "agreed"] as const) {
    assert.equal(issueOutcome({ refutation: null, tensions: [tension(status)] }), "none", `${status} は結論なし`);
  }
  assert.equal(issueOutcome({ refutation: null, tensions: [] }), "none", "対立なしも結論なし");
  console.log("  [ok] issueOutcome: 止揚 / 論破 / 結論なし");
}

// ── 結論なしの理由 ────────────────────────────────────────────────
{
  assert.equal(noConclusionReason(tension("synthesized")), null);
  assert.match(noConclusionReason(tension("compromised"), synthesis("t-compromised", "両方採用"))!, /折衷.*両方採用/);
  assert.match(noConclusionReason(tension("agreed_disagree"))!, /批准されず/);
  assert.match(noConclusionReason(tension("resolved", "A を支持"))!, /照会.*A を支持/);
  assert.match(noConclusionReason(tension("unresolved_fact", "売上データなし"))!, /未解決の事実問題: 売上データなし/);
  assert.match(noConclusionReason(tension("agreed", "同じ結論"))!, /旧方式の合意判定/);
  assert.equal(renderOutcomeLine("none", "理由"), "結論なし: 理由");
  console.log("  [ok] noConclusionReason: 理由を添える");
}

// ── 決着表に区分ラベル ────────────────────────────────────────────
{
  const names = new Map([["p-pro", "翔"], ["p-con", "凛"]]);
  const positions = [
    { id: "a", issueId: "i1", personaId: "p-pro", stance: "pro" as const, claim: "下げる", qualifier: null, grounds: [], values: [] },
    { id: "b", issueId: "i1", personaId: "p-con", stance: "con" as const, claim: "下げない", qualifier: null, grounds: [], values: [] },
  ];
  const synth = tension("synthesized");
  const aufheben = settlementToRoundSummary(
    { issue, positions, tensions: [synth], syntheses: [synthesis(synth.id, "層で分ける")], refutation: null },
    names,
    null
  );
  assert.match(aufheben.summary, /【止揚】/);
  assert.match(aufheben.summary, /止揚: 層で分ける/);
  assert.deepEqual(aufheben.aufhebung, ["層で分ける"]);

  const refuted = settlementToRoundSummary(
    { issue, positions, tensions: [], syntheses: [], refutation: "「下げない」は論破された" },
    names,
    0.5
  );
  assert.match(refuted.summary, /【論破】.*論破: 「下げない」は論破された/);

  const none = settlementToRoundSummary(
    { issue, positions, tensions: [tension("agreed_disagree")], syntheses: [], refutation: null },
    names,
    null
  );
  assert.match(none.summary, /【結論なし】.*結論なし: 止揚案が両陣営に批准されず/);
  assert.deepEqual(none.aufhebung, []);
  console.log("  [ok] settlementToRoundSummary: 区分ラベルと理由");
}

console.log("outcome tests: all passed");
