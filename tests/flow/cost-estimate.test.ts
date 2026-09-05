/**
 * cost_usd 補完テスト (サブスク/トークンのみ backend の等価コスト推定)。
 * - estimateCostUsd: モデルファミリ別単価 + cache レート + 未知モデル/空 usage
 * - withCostLog: cost_usd を返さない backend は token × 単価で補完、claude-cli の
 *   envelope cost_usd は上書きしない
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

// ── estimateCostUsd ──────────────────────────────────────────────
{
  const { estimateCostUsd, modelFamily } = await import("../../src/persona-engine/llm/pricing.js");

  assert.equal(modelFamily("claude-opus-5"), "opus");
  assert.equal(modelFamily("claude-sonnet-5"), "sonnet");
  assert.equal(modelFamily("claude-haiku-4-5"), "haiku");
  assert.equal(modelFamily("gemma4:12b"), null);
  assert.equal(modelFamily(undefined), null);
  console.log("  [ok] modelFamily 正規化");

  // opus: input 5 / output 25 / cacheRead 0.5 / cacheWrite 6.25 per MTok
  const cost = estimateCostUsd("claude-opus-5", {
    input_tokens: 1_000_000,
    output_tokens: 1_000_000,
    cache_read_input_tokens: 1_000_000,
    cache_creation_input_tokens: 1_000_000,
  });
  assert.ok(Math.abs(cost! - (5 + 25 + 0.5 + 6.25)) < 1e-9, `opus 4種合算 = 36.75, got ${cost}`);
  console.log("  [ok] estimateCostUsd opus (input+output+cache)");

  // token のみ (worker-pool 形): cache 無し
  const tokenOnly = estimateCostUsd("claude-opus-5", { input_tokens: 2_000_000, output_tokens: 0 });
  assert.ok(Math.abs(tokenOnly! - 10) < 1e-9, "2M input = $10");
  console.log("  [ok] estimateCostUsd token-only");

  // 未知モデル / 空 usage → undefined
  assert.equal(estimateCostUsd("gemma4:12b", { input_tokens: 100 }), undefined, "未知モデル → undefined");
  assert.equal(estimateCostUsd("claude-opus-5", {}), undefined, "空 usage → undefined");
  assert.equal(estimateCostUsd("claude-opus-5", undefined), undefined, "usage 無し → undefined");
  console.log("  [ok] estimateCostUsd 未知/空 → undefined");
}

// ── withCostLog の補完挙動 ────────────────────────────────────────
const TMP_DIR = path.resolve(".tmp/flow-cost-estimate-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });

{
  // (a) token はあるが cost_usd 無し (worker-pool / anthropic) → 推定で補完
  process.env.DATABASE_PATH = path.join(TMP_DIR, "estimate.db");
  const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
  _resetFlowDb();
  const { withCostLog } = await import("../../src/flow/cost-logger.js");
  const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");

  const mock = new MockLLMClient([
    () => ({ ok: true as const, text: "x", usage: { input_tokens: 1_000_000, output_tokens: 0 } }),
  ]);
  await withCostLog(mock, {
    flow: "discussion",
    sessionId: "S-wp",
    location: "utterance",
    backend: "worker-pool",
  }).invoke({ prompt: "p", model: "claude-opus-5" });

  const db = new Database(process.env.DATABASE_PATH);
  const row = db.prepare("SELECT * FROM llm_call_log ORDER BY id DESC LIMIT 1").get() as Record<string, unknown>;
  db.close();
  assert.ok(Math.abs((row.cost_usd as number) - 5) < 1e-9, `1M opus input → $5 推定, got ${row.cost_usd}`);
  console.log("  [ok] withCostLog: cost_usd 無し backend を推定補完");

  // (b) envelope cost_usd がある (claude-cli) → 上書きしない
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "envelope.db");
  const mock2 = new MockLLMClient([
    () => ({ ok: true as const, text: "x", usage: { input_tokens: 1_000_000, output_tokens: 0, cost_usd: 0.123 } }),
  ]);
  await withCostLog(mock2, {
    flow: "discussion",
    sessionId: "S-cli",
    location: "utterance",
    backend: "claude-cli",
  }).invoke({ prompt: "p", model: "claude-opus-5" });

  const db2 = new Database(process.env.DATABASE_PATH);
  const row2 = db2.prepare("SELECT * FROM llm_call_log ORDER BY id DESC LIMIT 1").get() as Record<string, unknown>;
  db2.close();
  assert.equal(row2.cost_usd, 0.123, "envelope cost_usd は推定で上書きしない");
  console.log("  [ok] withCostLog: envelope cost_usd を温存");

  // (c) 未知モデル (ローカル) → cost_usd NULL のまま
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "local.db");
  const mock3 = new MockLLMClient([
    () => ({ ok: true as const, text: "x", usage: { input_tokens: 500, output_tokens: 100 } }),
  ]);
  await withCostLog(mock3, {
    flow: "discussion",
    sessionId: "S-local",
    location: "utterance",
    backend: "local",
  }).invoke({ prompt: "p", model: "gemma4:12b" });

  const db3 = new Database(process.env.DATABASE_PATH);
  const row3 = db3.prepare("SELECT * FROM llm_call_log ORDER BY id DESC LIMIT 1").get() as Record<string, unknown>;
  db3.close();
  assert.equal(row3.cost_usd, null, "未知モデルは cost_usd NULL のまま");
  console.log("  [ok] withCostLog: 未知モデルは NULL");

  delete process.env.DATABASE_PATH;
  _resetFlowDb();
}

console.log("cost-estimate tests: all passed");
