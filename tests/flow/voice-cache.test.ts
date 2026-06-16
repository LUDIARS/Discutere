/**
 * 発話時 RAG キャッシュ テスト (item5)。
 * 同一テーマ/語の lookup はセッション内で 1 回に集約し、全ペルソナで使い回す。
 */

import assert from "node:assert/strict";

const { createVoiceCache } = await import("../../src/flow/voice-cache.js");

// ── 同一キーは 1 回だけ実 lookup、以降キャッシュ ─────────────────────────────
{
  let calls = 0;
  const cache = createVoiceCache((terms, limit) => {
    calls += 1;
    return [{ content: `${terms.join(",")}#${limit}`, source: "test" }];
  });

  const a = cache.lookup(["ヴァンサバ"], 5);
  const b = cache.lookup(["ヴァンサバ"], 5);
  assert.equal(calls, 1, "同一キーは 1 回だけ実 lookup");
  assert.deepEqual(a, b, "結果は同一参照/同値");
  assert.equal(cache.misses(), 1);
  assert.equal(cache.hits(), 1);

  // 語順/大文字小文字/空白を正規化して同一キー扱い
  cache.lookup([" ヴァンサバ "], 5);
  assert.equal(calls, 1, "正規化後は同一キー (再 lookup しない)");

  // limit が違えば別キー
  cache.lookup(["ヴァンサバ"], 3);
  assert.equal(calls, 2, "limit 違いは別 lookup");
  console.log("  [ok] メモ化 + キー正規化");
}

// ── fn 未注入なら常に空配列 (スタブ) ─────────────────────────────────────────
{
  const cache = createVoiceCache();
  assert.deepEqual(cache.lookup(["x"], 5), []);
  assert.deepEqual(cache.lookup(["x"], 5), []);
  assert.equal(cache.misses(), 1, "スタブでも初回だけ miss");
  console.log("  [ok] fn 未注入スタブ");
}

console.log("voice-cache tests: all passed");
