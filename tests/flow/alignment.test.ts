/**
 * すり合わせ (dialectic.md §4.5) テスト。
 * - parseAlignment: 不明 id を捨てる / 両方に挙がった id は未決優先 / 扱われなかった根拠を未決に補う
 * - generateAlignment: LLM 障害・パース失敗は全根拠を未決にして degrade
 * - alignmentStatus / renderAlignmentText
 */

import assert from "node:assert/strict";

const { parseAlignment, generateAlignment, alignmentStatus, renderAlignmentText, buildAlignmentPrompt } = await import(
  "../../src/flow/dialectic/alignment.js"
);

const pro = {
  id: "pos-pro",
  issueId: "i1",
  personaId: "p-pro",
  stance: "pro" as const,
  claim: "天井を下げると継続率が上がる",
  qualifier: "天井で離脱するライト層",
  grounds: [
    { id: "pro-G1", text: "離脱理由の 1 位が天井", state: "defended" as const, kind: "deduction" as const, warrant: "主因を緩めれば継続する" },
    { id: "pro-G2", text: "他ゲームで緩和後に継続率が上がった", state: "conceded" as const, kind: "induction" as const },
  ],
  values: [],
};
const con = {
  id: "pos-con",
  issueId: "i1",
  personaId: "p-con",
  stance: "con" as const,
  claim: "天井を下げると重課金層の支出が減る",
  qualifier: null,
  grounds: [{ id: "con-G1", text: "重課金層は天井まで回す", state: "unchallenged" as const }],
  values: [],
};
const tension = {
  id: "t1",
  issueId: "i1",
  positionA: "pos-pro",
  positionB: "pos-con",
  type: "values" as const,
  status: "open" as const,
  resolutionNote: null,
};

// ── プロンプト ────────────────────────────────────────────────────────────────

{
  const prompt = buildAlignmentPrompt({ issueTitle: "天井を下げるか", tension, positionA: pro, positionB: con });
  assert.ok(prompt.includes("# 指示 (すり合わせ)"), "すり合わせの指示が載る");
  assert.ok(prompt.includes("限定: 天井で離脱するライト層"), "主張の限定が載る");
  assert.ok(!prompt.includes("[pro-G2]"), "譲歩済みの根拠は載せない");
  assert.ok(!prompt.includes("止揚"), "止揚を求めない");
  console.log("  [ok] buildAlignmentPrompt: 体験/施策/他ゲーム + 合意/未決を求める");
}

// ── 検証 ──────────────────────────────────────────────────────────────────────

{
  const parsed = parseAlignment(
    JSON.stringify({
      experience: "欲しいキャラに近づける体験",
      measure: "天井を下げる",
      references: "他ゲームでは継続率が上がった",
      agreed: ["pro-G1", "con-G1", "pro-G2", "no-such"],
      open: [
        { groundId: "con-G1", point: "data", need: "重課金層の支出の推移" },
        { groundId: "no-such", point: "data", need: "捨てられる" },
        { groundId: null, point: "qualifier", need: "対象をライト層に限るか" },
        { groundId: "pro-G1", point: "warrant", need: "" },
      ],
      text: "まとめ",
    }),
    [pro, con]
  );
  assert.ok(parsed, "JSON を読める");
  assert.deepEqual(parsed!.agreed, ["pro-G1"], "不明 id・譲歩済み・未決と重複した id は合意に入れない");
  assert.deepEqual(
    parsed!.open.map((o) => o.groundId),
    ["con-G1", null],
    "不明 id と need の無い未決は捨て、限定そのものの未決 (null) は残す"
  );
  assert.equal(parsed!.open[1].point, "qualifier");

  const missing = parseAlignment(JSON.stringify({ agreed: [], open: [], text: "t" }), [pro, con]);
  assert.deepEqual(
    missing!.open.map((o) => o.groundId).sort(),
    ["con-G1", "pro-G1"],
    "扱われなかった未譲歩の根拠は未決に補う"
  );
  assert.equal(parseAlignment("JSON ではない", [pro, con]), null, "JSON でなければ null");
  console.log("  [ok] parseAlignment: id 検証 + 未決優先 + 取りこぼし補完");
}

// ── 生成 (degrade) ────────────────────────────────────────────────────────────

{
  const fail = { invoke: async () => ({ ok: false as const, error: "boom" }) };
  const warnings: string[] = [];
  const degraded = await generateAlignment({
    issueTitle: "天井を下げるか",
    tension,
    positionA: pro,
    positionB: con,
    paperSystem: "",
    llm: fail as any,
    warn: (m) => warnings.push(m),
  });
  assert.equal(degraded.degraded, true);
  assert.deepEqual(degraded.open.map((o) => o.groundId).sort(), ["con-G1", "pro-G1"], "全根拠を未決にする");
  assert.equal(warnings.length, 1, "warn を出す");

  const blankText = await generateAlignment({
    issueTitle: "天井を下げるか",
    tension,
    positionA: pro,
    positionB: con,
    paperSystem: "",
    llm: {
      invoke: async () => ({
        ok: true as const,
        text: JSON.stringify({ experience: "体験", measure: "施策", references: "", agreed: ["pro-G1", "con-G1"], open: [] }),
      }),
    } as any,
  });
  assert.equal(blankText.degraded, false);
  assert.ok(blankText.text.includes("根拠はすべて合意できました"), "露出文が空なら機械組み立て");
  assert.equal(alignmentStatus(blankText), "aligned", "未決が無ければ aligned");
  assert.equal(alignmentStatus(degraded), "partially_aligned", "未決があれば partially_aligned");
  assert.ok(renderAlignmentText({ experience: "", measure: "", references: "", open: [] }).includes("整理できた点はまだありません"));
  console.log("  [ok] generateAlignment: degrade + 露出文の機械組み立て + 状態");
}

console.log("alignment tests: all passed");
