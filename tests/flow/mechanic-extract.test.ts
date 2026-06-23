/**
 * メカニクス LLM 増補 (enrichMechanics) のテスト。
 * 感想を根拠に既存メカニクスへ追加抽出し、重複除去 + 目標件数クランプ + 失敗時 degrade を検証。
 */
import assert from "node:assert/strict";

import { enrichMechanics } from "../../src/flow/mechanic-extract.js";
import { MockLLMClient } from "../../src/persona-engine/llm/mock.js";
import type { MechanicSummary } from "../../src/flow/investigate.js";

const existing: MechanicSummary[] = [
  { name: "引っ張りショット", description: "敵に引っ張って当てる" },
];
const voices = [
  { content: "友情コンボが派手で気持ちいい", source: "niconico" },
  { content: "運極の周回がだるい", source: "youtube" },
];

// ── 正常: 追加メカニクスを抽出してマージ ────────────────────────────────────
{
  const llm = new MockLLMClient([
    () =>
      JSON.stringify([
        { name: "友情コンボ", description: "味方の固有技が連鎖発動", intended_affect: "爽快感" },
        { name: "運極周回", description: "同キャラを集めて強化", intended_affect: "達成感" },
        { name: "引っ張りショット", description: "重複 (既存)" }, // 既存と重複 → 除外される
      ]),
  ]);
  const out = await enrichMechanics({ theme: "モンスト", existing, voices, llm, target: 30 });
  assert.equal(out.length, 3, "既存1 + 追加2 (重複1除外)");
  assert.equal(out[0].name, "引っ張りショット", "既存を先頭に保持");
  assert.ok(out.some((m) => m.name === "友情コンボ"));
  assert.ok(out.some((m) => m.name === "運極周回"));
}
console.log("ok enrichMechanics merge + dedupe");

// ── 目標件数でクランプ ──────────────────────────────────────────────────────
{
  const many = Array.from({ length: 20 }, (_, i) => ({ name: `M${i}`, description: "d" }));
  const llm = new MockLLMClient([() => JSON.stringify(many)]);
  const out = await enrichMechanics({ theme: "x", existing: [], voices, llm, target: 5 });
  assert.equal(out.length, 5, "target=5 でクランプ");
}
console.log("ok enrichMechanics target clamp");

// ── 既存が目標以上なら LLM を呼ばない ───────────────────────────────────────
{
  const full = Array.from({ length: 30 }, (_, i) => ({ name: `E${i}`, description: "d" }));
  const llm = new MockLLMClient([
    () => {
      throw new Error("LLM must not be called when already at target");
    },
  ]);
  const out = await enrichMechanics({ theme: "x", existing: full, voices, llm, target: 30 });
  assert.equal(out.length, 30);
  assert.equal(llm.calls, 0, "目標達成済みなら LLM 呼び出し 0");
}
console.log("ok enrichMechanics skips LLM when full");

// ── パース不能 / LLM 失敗は既存のまま degrade ──────────────────────────────
{
  const llm = new MockLLMClient([() => "JSONじゃない返答"]);
  const out = await enrichMechanics({ theme: "x", existing, voices, llm, target: 30 });
  assert.deepEqual(out, existing, "解析失敗時は既存メカニクスを壊さない");

  const errLlm = new MockLLMClient([() => ({ ok: false, error: "boom" })]);
  const out2 = await enrichMechanics({ theme: "x", existing, voices, llm: errLlm, target: 30 });
  assert.deepEqual(out2, existing, "LLM 失敗時も既存のまま");
}
console.log("ok enrichMechanics graceful degrade");

console.log("mechanic-extract.test.ts: all passed");
