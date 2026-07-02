/**
 * 議論適性ゲート (09-paper-gate-debatability) のテスト。
 *   - 証拠バランスは機械計算のみ (LLM 不使用) — 極性 9:1 で polaritySkew 検出
 *   - 争点 1 個 → 議論不適 + 「壁打ち」再提案 / 材料不足 → 「学習」再提案
 *   - armable=both が閾値以上 → 議論適性あり
 *   - LLM 失敗 → degraded=true で「適性あり扱い」(ゲート失敗で議論を止めない)
 *   - decomposeIssues のクランプ / buildPaperDraft への `# 論点` 前倒し
 */

import assert from "node:assert/strict";

import {
  assessDebatability,
  computeEvidenceBalance,
  annotatedIssues,
  type DebatabilityVoice,
} from "../../src/flow/debatability.js";
import { decomposeIssues } from "../../src/flow/dialectic/issues.js";
import { buildPaperDraft } from "../../src/flow/paper-review.js";
import { markdownToPaperDraft } from "../../src/flow/paper-markdown.js";
import { MockLLMClient } from "../../src/persona-engine/llm/mock.js";

/** テスト用の決定論的極性ベクトル (textToVector 互換: dims[0] = (valence+1)/2)。 */
function stubVector(text: string): number[] {
  const v = text.includes("好き") ? 1 : text.includes("嫌い") ? -1 : 0;
  return [(v + 1) / 2, ...Array(19).fill(0)];
}

function voicesSkewed(pos: number, neg: number, source = "niconico"): DebatabilityVoice[] {
  return [
    ...Array.from({ length: pos }, (_, i) => ({ content: `好き ${i}`, source })),
    ...Array.from({ length: neg }, (_, i) => ({ content: `嫌い ${i}`, source })),
  ];
}

// ── 証拠バランス: 極性 9:1 で polaritySkew 検出 (機械計算・LLM 不使用) ───────
{
  const e = computeEvidenceBalance(voicesSkewed(9, 1), stubVector);
  assert.equal(e.voiceCount, 10);
  assert.equal(e.positive, 9);
  assert.equal(e.negative, 1);
  assert.ok(Math.abs(e.polaritySkew - 0.8) < 1e-9, `skew=0.8 を検出 (got ${e.polaritySkew})`);
  assert.equal(e.dominantSource?.name, "niconico");
  assert.ok(Math.abs((e.dominantSource?.share ?? 0) - 1) < 1e-9, "単一ソース占有 100%");
}
{
  const e = computeEvidenceBalance([], stubVector);
  assert.equal(e.voiceCount, 0);
  assert.equal(e.polaritySkew, 0);
  assert.equal(e.dominantSource, null);
}
console.log("ok debatability 証拠バランス (機械計算)");

// ── 争点 1 個 (材料はある) → 議論不適 + 壁打ち提案 ──────────────────────────
{
  const llm = new MockLLMClient([
    () => JSON.stringify(["ガチャ緩和は収益を毀損するか"]), // decomposeIssues
    () => JSON.stringify([{ index: 1, armable: "both" }]), // armability
  ]);
  const r = await assessDebatability({
    theme: "ガチャ",
    voices: voicesSkewed(5, 5), // 均衡 = 材料は十分
    llm,
    minArmableIssues: 2,
    toVector: stubVector,
  });
  assert.equal(r.degraded, false);
  assert.equal(r.issues.length, 1);
  assert.equal(r.armableBothCount, 1);
  assert.equal(r.debatable, false, "armable=both 1 < 2 → 議論不適");
  assert.equal(r.recommendation?.flow, "sparring", "争点 0〜1 (材料あり) → 壁打ち");
}
console.log("ok debatability 争点 1 → 壁打ち再提案");

// ── 材料不足 (極性偏り) が主因 → 学習提案 (足りない側を提示) ─────────────────
{
  const llm = new MockLLMClient([
    () => JSON.stringify(["論点A", "論点B", "論点C"]),
    () =>
      JSON.stringify([
        { index: 1, armable: "pro-only" },
        { index: 2, armable: "pro-only" },
        { index: 3, armable: "both" },
      ]),
  ]);
  const r = await assessDebatability({
    theme: "T",
    voices: voicesSkewed(9, 1),
    llm,
    minArmableIssues: 2,
    toVector: stubVector,
  });
  assert.equal(r.debatable, false);
  assert.equal(r.recommendation?.flow, "learning", "極性偏り → 学習提案");
  assert.ok(r.recommendation?.reason.includes("否定・批判側"), "足りない側 (否定側) を提示");
}
console.log("ok debatability 材料不足 → 学習再提案");

// ── armable=both >= 閾値 → 議論適性あり (再提案なし) ─────────────────────────
{
  const llm = new MockLLMClient([
    () => JSON.stringify(["論点A", "論点B"]),
    () =>
      JSON.stringify([
        { index: 1, armable: "both" },
        { index: 2, armable: "both" },
      ]),
  ]);
  const r = await assessDebatability({
    theme: "T",
    voices: voicesSkewed(5, 5),
    llm,
    minArmableIssues: 2,
    toVector: stubVector,
  });
  assert.equal(r.debatable, true);
  assert.equal(r.recommendation, null);
  const annotated = annotatedIssues(r);
  assert.ok(annotated[0].endsWith("(両論: ○)"), "両論注記が付く");
}
console.log("ok debatability 適性あり");

// ── LLM 失敗 → degraded=true で議論を止めない (適性あり扱い) ─────────────────
{
  const llm = new MockLLMClient([() => ({ ok: false, error: "boom" })]);
  const warns: string[] = [];
  const r = await assessDebatability({
    theme: "T",
    voices: [],
    llm,
    minArmableIssues: 2,
    toVector: stubVector,
    warn: (m) => warns.push(m),
  });
  assert.equal(r.degraded, true);
  assert.equal(r.debatable, true, "degrade 時は議論を止めない");
  assert.ok(warns.length > 0, "degrade は warn で明示する (握り潰さない)");
}
// 両論武装判定だけ失敗した場合も degrade (issues は保持)
{
  const llm = new MockLLMClient([
    () => JSON.stringify(["論点A", "論点B", "論点C"]),
    () => ({ ok: false, error: "armability down" }),
  ]);
  const r = await assessDebatability({
    theme: "T",
    voices: voicesSkewed(3, 3),
    llm,
    minArmableIssues: 2,
    toVector: stubVector,
  });
  assert.equal(r.degraded, true);
  assert.equal(r.debatable, true);
  assert.equal(r.issues.length, 3, "分解済み論点は保持する");
}
console.log("ok debatability LLM 失敗 degrade");

// ── decomposeIssues: 5 個クランプ + 失敗は throw (握り潰さない) ──────────────
{
  const llm = new MockLLMClient([() => JSON.stringify(["1", "2", "3", "4", "5", "6", "7"])]);
  const issues = await decomposeIssues({ theme: "T", llm });
  assert.equal(issues.length, 5, "最大 5 個にクランプ");
}
{
  const llm = new MockLLMClient([() => ({ ok: false, error: "down" })]);
  await assert.rejects(() => decomposeIssues({ theme: "T", llm }), /論点分解 LLM エラー/);
}
{
  const llm = new MockLLMClient([() => "[]"]);
  const issues = await decomposeIssues({ theme: "T", llm });
  assert.deepEqual(issues, [], "争点なしは空配列 (失敗と区別)");
}
console.log("ok decomposeIssues クランプ/失敗 throw");

// ── buildPaperDraft: 論点分解の前倒し (`# 論点` 節) + info.debatability ──────
{
  const gateLlm = new MockLLMClient([
    () => JSON.stringify(["ガチャ緩和は収益を毀損するか", "天井はライト層の課金動機を下げるか"]),
    () =>
      JSON.stringify([
        { index: 1, armable: "both" },
        { index: 2, armable: "both" },
      ]),
  ]);
  const { draft, info } = await buildPaperDraft("架空のテーマXYZ", [], {
    gamesDir: "./nonexistent-games-dir",
    listExternalVoices: () => voicesSkewed(3, 3).map((v) => ({ content: v.content, source: v.source ?? "x" })),
    debatability: { llm: gateLlm, minArmableIssues: 2 },
  });
  assert.ok(draft.bodyMd.includes("# 論点"), "草案 md に # 論点 節が前倒しされる");
  assert.ok(draft.bodyMd.includes("1. ガチャ緩和は収益を毀損するか (両論: ○)"), "両論注記付き");
  assert.equal(info.debatability?.debatable, true);
  // round-trip: `# 論点` 節 → issues[] (両論注記は剥がれる)
  const back = markdownToPaperDraft(draft.bodyMd, draft);
  assert.deepEqual(back.issues, [
    "ガチャ緩和は収益を毀損するか",
    "天井はライト層の課金動機を下げるか",
  ]);
}
// ゲート未指定 (enabled=false 相当) → 現行挙動 (論点節なし・info.debatability なし)
{
  const { draft, info } = await buildPaperDraft("架空のテーマXYZ", [], {
    gamesDir: "./nonexistent-games-dir",
    listExternalVoices: () => [],
  });
  assert.ok(!draft.bodyMd.includes("# 論点"), "ゲート無効なら論点節を作らない");
  assert.equal(info.debatability, undefined);
}
console.log("ok buildPaperDraft 論点前倒し + ゲート無効時の現行挙動");

console.log("debatability tests: all ok");
