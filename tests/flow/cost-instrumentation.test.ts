/**
 * LLM コスト計装テスト。
 * - parseClaudeCliResult (json エンベロープ: 配列 / 単一 / エラー / パース失敗)
 * - flow_0010 マイグレーション (cost 列追加)
 * - withCostLog が cache/cost 列を記録する
 * - summarizeCost の集計
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

// ── parseClaudeCliResult ─────────────────────────────────────────

{
  const { parseClaudeCliResult } = await import(
    "../../src/persona-engine/llm/claude-cli.js"
  );

  // (a) イベント配列 (末尾に type:"result")。実際の claude -p --output-format json 形。
  const arrEnvelope = JSON.stringify([
    { type: "system", subtype: "init" },
    {
      type: "result",
      subtype: "success",
      is_error: false,
      result: "  Hi  ",
      total_cost_usd: 0.0620745,
      usage: {
        input_tokens: 2,
        output_tokens: 5,
        cache_read_input_tokens: 39804,
        cache_creation_input_tokens: 6726,
      },
    },
  ]);
  const r1 = parseClaudeCliResult(arrEnvelope);
  assert.ok(r1.ok, "array envelope parsed ok");
  assert.equal(r1.ok && r1.text, "Hi", "result text trimmed");
  assert.equal(r1.ok && r1.usage?.input_tokens, 2);
  assert.equal(r1.ok && r1.usage?.cache_read_input_tokens, 39804);
  assert.equal(r1.ok && r1.usage?.cache_creation_input_tokens, 6726);
  assert.equal(r1.ok && r1.usage?.cost_usd, 0.0620745, "total_cost_usd → cost_usd");
  console.log("  [ok] parseClaudeCliResult: array envelope + cache/cost");

  // (b) 単一オブジェクト形。
  const objEnvelope = JSON.stringify({
    type: "result",
    subtype: "success",
    result: "one word",
    total_cost_usd: 0.001,
    usage: { input_tokens: 3, output_tokens: 2 },
  });
  const r2 = parseClaudeCliResult(objEnvelope);
  assert.ok(r2.ok, "object envelope parsed ok");
  assert.equal(r2.ok && r2.text, "one word");
  assert.equal(r2.ok && r2.usage?.cost_usd, 0.001);
  assert.equal(
    r2.ok && r2.usage?.cache_read_input_tokens,
    undefined,
    "missing cache fields → undefined"
  );
  console.log("  [ok] parseClaudeCliResult: object envelope");

  // (c) is_error / subtype != success → ok:false
  const errEnvelope = JSON.stringify([
    { type: "result", subtype: "error_max_turns", is_error: true, result: "boom" },
  ]);
  const r3 = parseClaudeCliResult(errEnvelope);
  assert.ok(!r3.ok, "error subtype → ok:false");
  assert.match(r3.ok ? "" : r3.error, /error_max_turns/, "error subtype surfaced");
  console.log("  [ok] parseClaudeCliResult: error subtype");

  // (d) パース失敗 → ok:false
  const r4 = parseClaudeCliResult("not json at all");
  assert.ok(!r4.ok, "invalid json → ok:false");
  assert.match(r4.ok ? "" : r4.error, /json parse failed/, "parse error surfaced");
  console.log("  [ok] parseClaudeCliResult: parse failure");

  // (e) result 本文が空 → ok:false
  const emptyEnvelope = JSON.stringify({ type: "result", subtype: "success", result: "   " });
  const r5 = parseClaudeCliResult(emptyEnvelope);
  assert.ok(!r5.ok, "empty result → ok:false");
  console.log("  [ok] parseClaudeCliResult: empty result text");
}

// ── マイグレーション (flow_0010) ──────────────────────────────────

const TMP_DIR = path.resolve(".tmp/flow-cost-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });

{
  const dbPath = path.join(TMP_DIR, "migrate.db");
  const db = new Database(dbPath);
  const { applyFlowMigrations } = await import("../../src/flow/db/migrations.js");
  applyFlowMigrations(db);

  const cols = (
    db.prepare("PRAGMA table_info(llm_call_log)").all() as Array<{ name: string }>
  ).map((c) => c.name);
  assert.ok(cols.includes("cache_read_input_tokens"), "cache_read_input_tokens column");
  assert.ok(cols.includes("cache_creation_input_tokens"), "cache_creation_input_tokens column");
  assert.ok(cols.includes("cost_usd"), "cost_usd column");

  // 冪等
  applyFlowMigrations(db);
  db.close();
  console.log("  [ok] flow_0010 adds cost columns (idempotent)");
}

// ── withCostLog が cache/cost を記録 ──────────────────────────────

{
  const dbPath = path.join(TMP_DIR, "costlog.db");
  process.env.DATABASE_PATH = dbPath;

  const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
  _resetFlowDb();

  const { withCostLog } = await import("../../src/flow/cost-logger.js");
  const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");

  const mock = new MockLLMClient([
    () => ({
      ok: true as const,
      text: "hello",
      usage: {
        input_tokens: 2,
        output_tokens: 5,
        cache_read_input_tokens: 39804,
        cache_creation_input_tokens: 6726,
        cost_usd: 0.0620745,
      },
    }),
  ]);

  const wrapped = withCostLog(mock, {
    flow: "discussion",
    sessionId: "sess-cost",
    location: "utterance",
    backend: "claude-cli",
  });
  await wrapped.invoke({ prompt: "p", model: "claude-opus-4-8" });

  const db = new Database(dbPath);
  const row = db
    .prepare("SELECT * FROM llm_call_log ORDER BY id DESC LIMIT 1")
    .get() as Record<string, unknown>;
  db.close();

  assert.equal(row.cache_read_input_tokens, 39804, "cache_read recorded");
  assert.equal(row.cache_creation_input_tokens, 6726, "cache_creation recorded");
  assert.equal(row.cost_usd, 0.0620745, "cost_usd recorded");
  console.log("  [ok] withCostLog records cache/cost columns");

  // usage に cache/cost が無い backend → NULL
  _resetFlowDb();
  const dbPath2 = path.join(TMP_DIR, "costlog-null.db");
  process.env.DATABASE_PATH = dbPath2;
  const mock2 = new MockLLMClient([
    () => ({ ok: true as const, text: "x", usage: { input_tokens: 1, output_tokens: 1 } }),
  ]);
  await withCostLog(mock2, {
    flow: "discussion",
    sessionId: "sess-null",
    location: "utterance",
  }).invoke({ prompt: "p" });

  const db2 = new Database(dbPath2);
  const row2 = db2
    .prepare("SELECT * FROM llm_call_log ORDER BY id DESC LIMIT 1")
    .get() as Record<string, unknown>;
  db2.close();
  assert.equal(row2.cache_read_input_tokens, null, "cache_read null when absent");
  assert.equal(row2.cost_usd, null, "cost_usd null when absent");
  console.log("  [ok] withCostLog leaves cache/cost NULL when backend omits them");

  // worker-pool: usage.model (transcript 由来) が args.model 不在でも記録列と単価引きを駆動する (#246)
  _resetFlowDb();
  const dbPath3 = path.join(TMP_DIR, "costlog-wp-model.db");
  process.env.DATABASE_PATH = dbPath3;
  const mock3 = new MockLLMClient([
    () => ({
      ok: true as const,
      text: "y",
      usage: {
        input_tokens: 1000,
        output_tokens: 200,
        model: "claude-haiku-4-5-20251001",
      },
    }),
  ]);
  // args.model を渡さない (非 claude provider / worker-pool で起こる状況)。
  await withCostLog(mock3, {
    flow: "discussion",
    sessionId: "sess-wp",
    location: "utterance",
    backend: "worker-pool",
  }).invoke({ prompt: "p" });

  const db3 = new Database(dbPath3);
  const row3 = db3
    .prepare("SELECT * FROM llm_call_log ORDER BY id DESC LIMIT 1")
    .get() as Record<string, unknown>;
  db3.close();
  assert.equal(row3.model, "claude-haiku-4-5-20251001", "usage.model recorded when args.model absent");
  // haiku 単価: input 1/MTok, output 5/MTok → 1000*1e-6 + 200*5e-6 = 0.001 + 0.001 = 0.002
  assert.ok(
    typeof row3.cost_usd === "number" && Math.abs((row3.cost_usd as number) - 0.002) < 1e-9,
    "cost estimated from usage.model haiku rate (not NULL)"
  );
  console.log("  [ok] withCostLog prices worker-pool rows via usage.model (#246)");
}

// ── summarizeCost 集計 ───────────────────────────────────────────

{
  const dbPath = path.join(TMP_DIR, "report.db");
  process.env.DATABASE_PATH = dbPath;

  const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
  _resetFlowDb();
  const { withCostLog } = await import("../../src/flow/cost-logger.js");
  const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");

  const mk = (cost: number) =>
    new MockLLMClient([
      () => ({
        ok: true as const,
        text: "t",
        usage: {
          input_tokens: 10,
          output_tokens: 4,
          cache_read_input_tokens: 100,
          cache_creation_input_tokens: 20,
          cost_usd: cost,
        },
      }),
    ]);

  await withCostLog(mk(0.01), {
    flow: "discussion",
    sessionId: "S1",
    location: "utterance",
    backend: "claude-cli",
  }).invoke({ prompt: "p" });
  await withCostLog(mk(0.02), {
    flow: "discussion",
    sessionId: "S1",
    location: "utterance",
    backend: "claude-cli",
  }).invoke({ prompt: "p" });
  await withCostLog(mk(0.005), {
    flow: "improvement",
    sessionId: "S2",
    location: "facilitator",
    backend: "anthropic",
  }).invoke({ prompt: "p" });

  const { summarizeCost } = await import("../../src/flow/cost-report.js");
  const db = new Database(dbPath);
  const report = summarizeCost(db);

  assert.equal(report.total.calls, 3, "total calls");
  assert.ok(Math.abs(report.total.costUsd - 0.035) < 1e-9, "total cost summed");
  assert.equal(report.total.cacheReadTokens, 300, "total cache read summed");

  const s1 = report.bySession.find((r) => r.key === "S1");
  assert.ok(s1, "S1 present");
  assert.equal(s1!.calls, 2, "S1 calls");
  assert.ok(Math.abs(s1!.costUsd - 0.03) < 1e-9, "S1 cost");

  // フィルタ: 単一セッション
  const onlyS2 = summarizeCost(db, { sessionId: "S2" });
  assert.equal(onlyS2.total.calls, 1, "filter session S2 → 1 call");
  assert.ok(Math.abs(onlyS2.total.costUsd - 0.005) < 1e-9, "filter S2 cost");

  const byBackend = report.byBackend.find((r) => r.key === "claude-cli");
  assert.equal(byBackend!.calls, 2, "claude-cli backend 2 calls");

  db.close();
  console.log("  [ok] summarizeCost aggregates by session/flow/backend + filter");

  delete process.env.DATABASE_PATH;
  _resetFlowDb();
}
