/**
 * ② 仕様書の解析学習 (spec-analyze.ts) テスト。
 * - 仕様書テキスト → LLM → GameMechanicEntry[] 抽出 + 正規化 (valence ホワイトリスト / 配列)
 * - 空 specText は LLM を呼ばず []
 * - LLM 失敗 / JSON 解析不能は [] で degrade (学習を止めない)
 * - コードフェンス/前置き混入に耐える (extractJsonArray 共有) + name で重複除去
 */

import assert from "node:assert/strict";
import { MockLLMClient } from "../../src/persona-engine/llm/mock.js";
import { analyzeSpecMechanics } from "../../src/flow/spec-analyze.js";

// ── 正常抽出 + 正規化 ────────────────────────────────────────────────────────
const okLlm = new MockLLMClient([
  () =>
    JSON.stringify([
      {
        name: "連鎖コンボ",
        description: "連鎖でスコアが伸びる",
        intended_affect: "爽快感",
        intended_valence: "POSITIVE",
        intended_aspects: ["fun", ""],
        intended_emotions: ["joy"],
      },
      { name: "ガチャ", description: "確率で強キャラ", intended_valence: "ふつう" },
      { name: "", description: "名前なしは捨てる" },
    ]),
]);
const mechs = await analyzeSpecMechanics({ theme: "Test Game", specText: "## 仕様\n連鎖でスコア…", llm: okLlm });
assert.equal(mechs.length, 2, "name のある 2 件 (空 name は除外)");
assert.equal(mechs[0].name, "連鎖コンボ");
assert.equal(mechs[0].intended_valence, "positive", "valence は小文字化してホワイトリスト一致");
assert.deepEqual(mechs[0].intended_aspects, ["fun"], "空文字を除いた配列");
assert.equal(mechs[1].intended_valence, undefined, "ホワイトリスト外の valence は落とす");
assert.equal(okLlm.calls, 1, "LLM は 1 回");
console.log("  [ok] spec-analyze: 仕様書 → メカニクス抽出 + 正規化");

// ── 空 specText は LLM を呼ばない ───────────────────────────────────────────
const guard = new MockLLMClient([
  () => {
    throw new Error("空 specText で LLM を呼んではいけない");
  },
]);
const empty = await analyzeSpecMechanics({ theme: "T", specText: "   ", llm: guard });
assert.deepEqual(empty, [], "空 specText は []");
assert.equal(guard.calls, 0, "LLM 呼び出し 0 回");
console.log("  [ok] spec-analyze: 空 specText は LLM を呼ばず []");

// ── LLM 失敗は [] で degrade ─────────────────────────────────────────────────
const failLlm = new MockLLMClient([() => ({ ok: false, error: "boom" })]);
const failed = await analyzeSpecMechanics({ theme: "T", specText: "spec", llm: failLlm, warn: () => {} });
assert.deepEqual(failed, [], "LLM 失敗時は [] (学習を止めない)");
console.log("  [ok] spec-analyze: LLM 失敗は [] で degrade");

// ── コードフェンス混入 + 重複除去 ───────────────────────────────────────────
const fencedLlm = new MockLLMClient([
  () =>
    "解析結果はこちらです:\n```json\n" +
    JSON.stringify([
      { name: "ジャンプ", description: "跳ぶ" },
      { name: "ジャンプ", description: "重複 (捨てる)" },
      { name: "ダッシュ", description: "走る" },
    ]) +
    "\n```\n以上です。",
]);
const fenced = await analyzeSpecMechanics({ theme: "T", specText: "spec", llm: fencedLlm });
assert.equal(fenced.length, 2, "コードフェンス耐性 + name 重複除去 (ジャンプ/ダッシュ)");
assert.deepEqual(fenced.map((m) => m.name), ["ジャンプ", "ダッシュ"]);
console.log("  [ok] spec-analyze: コードフェンス耐性 + 重複除去");

// ── maxMechanics で件数を切る ───────────────────────────────────────────────
const manyLlm = new MockLLMClient([
  () => JSON.stringify([{ name: "A" }, { name: "B" }, { name: "C" }]),
]);
const capped = await analyzeSpecMechanics({ theme: "T", specText: "spec", llm: manyLlm, maxMechanics: 2 });
assert.equal(capped.length, 2, "maxMechanics で 2 件にクランプ");
console.log("  [ok] spec-analyze: maxMechanics でクランプ");

// ── 長文 specText はチャンク分割して LLM を複数回呼ぶ ─────────────────────
{
  const chunkLlm = new MockLLMClient([
    // チャンク 1 の応答
    () => JSON.stringify([{ name: "チャンク1-A" }, { name: "チャンク1-B" }]),
    // チャンク 2 の応答
    () => JSON.stringify([{ name: "チャンク1-B" }, { name: "チャンク2-C" }]), // 重複あり
  ]);
  // 16000 文字超の specText (2チャンク分)
  const longSpec = "a".repeat(18000);
  const warns: string[] = [];
  const chunked = await analyzeSpecMechanics({
    theme: "T",
    specText: longSpec,
    llm: chunkLlm,
    warn: (m) => warns.push(m),
  });
  assert.ok(chunkLlm.calls >= 2, "長文は LLM を複数回呼ぶ");
  assert.ok(warns.some((w) => w.includes("チャンク")), "チャンク分割のwarnが出る");
  // 重複除去: チャンク1-A / チャンク1-B / チャンク2-C = 3件
  assert.equal(chunked.length, 3, "チャンク間の重複は除去される");
  assert.ok(chunked.map((m) => m.name).includes("チャンク2-C"), "複数チャンクから結果がマージされる");
}
console.log("  [ok] spec-analyze: 長文チャンク分割 + マージ + 重複除去");

console.log("spec-analyze (②) tests: all passed");
