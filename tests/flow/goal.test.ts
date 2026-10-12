/**
 * 論破の判定 (dialectic.md §4.5) テスト。
 * - 論破: 根拠がすべて崩れた (challenged / conceded) 側を検出、片側・双方・無し
 */

import assert from "node:assert/strict";

const { detectRefutation, standingGroundIds } =
  await import("../../src/flow/dialectic/goal.js");

function position(id: string, stance: "pro" | "con", states: string[], qualifier: string | null = null) {
  return {
    id,
    issueId: "i1",
    personaId: `p-${stance}`,
    stance,
    claim: `${stance} の主張`,
    qualifier,
    grounds: states.map((state, i) => ({ id: `${stance}-G${i + 1}`, text: `根拠 ${i + 1}`, state: state as any })),
    values: [],
  };
}

const issue = { id: "i1", sessionId: "s1", title: "天井を下げるか", ordinal: 1, source: "llm" as const, status: "open" as const };

// ── 論破 ──────────────────────────────────────────────────────────────────────

{
  const pro = position("a", "pro", ["defended", "challenged"], "ライト層");
  const con = position("b", "con", ["challenged", "conceded"]);
  assert.deepEqual(standingGroundIds(pro), ["pro-G1"], "defended は残る、challenged は崩れる");
  const one = detectRefutation(pro, con);
  assert.deepEqual(one.refuted.map((p) => p.id), ["b"], "根拠がすべて崩れた側が論破される");
  assert.ok(one.note!.includes("論破") && one.note!.includes("pro-G1"), "残った側と根拠が判定文に載る");

  const none = detectRefutation(pro, position("c", "con", ["unchallenged"]));
  assert.equal(none.note, null, "両方に根拠が残れば論破ではない");

  const both = detectRefutation(position("d", "pro", ["challenged"]), position("e", "con", ["conceded"]));
  assert.equal(both.refuted.length, 2, "双方崩れれば 2 件");
  assert.ok(both.note!.includes("双方"));
  console.log("  [ok] detectRefutation: 片側・双方・無し");
}

console.log("goal tests: all passed");
