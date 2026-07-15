/**
 * T1 共通基盤テスト。
 * - config flow セクション
 * - canCollectExternal / paperSupplement
 * - DB マイグレーション (新規 DB / 既存 DB 冪等)
 * - withCostLog
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

// ── config ──────────────────────────────────────────────────────

{
  // モジュールキャッシュをリセットしてから loadConfig を確認
  // (node test では同プロセスでキャッシュされる場合があるため直接 loadConfig を叩く)
  const { loadConfig } = await import("../../src/config.js");
  const cfg = loadConfig();

  assert.equal(cfg.flow.rounds, 3, "flow.rounds default");
  assert.equal(cfg.flow.turnsPerRound, 6, "flow.turnsPerRound default");
  assert.equal(cfg.flow.personaCount, 4, "flow.personaCount default");
  assert.equal(cfg.flow.voterCount, 3, "flow.voterCount default");
  assert.deepEqual(cfg.flow.tags, [], "flow.tags default");
  assert.equal(cfg.flow.sparringMaxTurns, 50, "flow.sparringMaxTurns default");
  assert.equal(cfg.flow.youtubeMaxComments, 200, "flow.youtubeMaxComments default");
  console.log("  [ok] config flow default values");
}

{
  // env override テスト
  process.env.DISCUTERE_FLOW_ROUNDS = "7";
  process.env.DISCUTERE_FLOW_TURNS_PER_ROUND = "10";
  process.env.DISCUTERE_FLOW_TAGS = "機密,開発";

  // loadConfig は cached を返さないよう直接インポートして再評価
  // キャッシュ回避のためファイルを直接読み込む (E2E テストでは getConfig() のキャッシュに注意)
  const { loadConfig: loadConfig2 } = await import("../../src/config.js?env=test");
  const cfg2 = loadConfig2();

  assert.equal(cfg2.flow.rounds, 7, "flow.rounds env override");
  assert.equal(cfg2.flow.turnsPerRound, 10, "flow.turnsPerRound env override");
  assert.deepEqual(cfg2.flow.tags, ["機密", "開発"], "flow.tags env override");

  // クリーンアップ
  delete process.env.DISCUTERE_FLOW_ROUNDS;
  delete process.env.DISCUTERE_FLOW_TURNS_PER_ROUND;
  delete process.env.DISCUTERE_FLOW_TAGS;
  console.log("  [ok] config flow env override");
}

// ── tags ─────────────────────────────────────────────────────────

{
  const { canCollectExternal, paperSupplement } = await import("../../src/flow/tags.js");

  assert.equal(canCollectExternal([]), true, "empty tags → collect allowed");
  assert.equal(canCollectExternal(["開発"]), true, "開発 → collect allowed");
  assert.equal(canCollectExternal(["運用"]), true, "運用 → collect allowed");
  assert.equal(canCollectExternal(["内部"]), false, "内部 → collect denied");
  assert.equal(canCollectExternal(["機密"]), false, "機密 → collect denied");
  assert.equal(canCollectExternal(["開発", "機密"]), false, "開発+機密 → collect denied");
  assert.equal(canCollectExternal(["開発", "内部"]), false, "開発+内部 → collect denied");
  console.log("  [ok] canCollectExternal");

  assert.equal(paperSupplement([]), "", "no tags → no supplement");
  assert.match(paperSupplement(["開発"]), /開発者観点/, "開発 → dev supplement");
  assert.match(paperSupplement(["運用"]), /運用者観点/, "運用 → ops supplement");
  assert.match(paperSupplement(["機密"]), /機密/, "機密 → confidential supplement");
  const multi = paperSupplement(["開発", "運用"]);
  assert.match(multi, /開発者観点/, "multi: 開発 present");
  assert.match(multi, /運用者観点/, "multi: 運用 present");
  console.log("  [ok] paperSupplement");
}

// ── DB マイグレーション ───────────────────────────────────────────

const TMP_DIR = path.resolve(".tmp/flow-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });

{
  // 新規 DB に適用
  const dbPath = path.join(TMP_DIR, "new.db");
  const db = new Database(dbPath);
  const { applyFlowMigrations } = await import("../../src/flow/db/migrations.js");
  applyFlowMigrations(db);

  const tables = (
    db.prepare("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name").all() as Array<{ name: string }>
  ).map((r) => r.name);

  assert.ok(tables.includes("discussion_paper"), "discussion_paper created");
  assert.ok(tables.includes("flow_actor_trace"), "GLab Cernere actor trace created");
  assert.ok(tables.includes("discussion_paper_round"), "discussion_paper_round created");
  assert.ok(tables.includes("vote"), "vote created");
  assert.ok(tables.includes("llm_call_log"), "llm_call_log created");
  assert.ok(tables.includes("_flow_migrations"), "_flow_migrations created");
  const paperColumns = (
    db.prepare("PRAGMA table_info(discussion_paper)").all() as Array<{ name: string }>
  ).map((r) => r.name);
  assert.ok(paperColumns.includes("discussion_no"), "discussion_paper has discussion_no");

  const migration = db
    .prepare("SELECT id FROM _flow_migrations WHERE id = 'flow_0001_initial'")
    .get() as { id: string } | undefined;
  assert.ok(migration, "migration record inserted");
  db.close();
  console.log("  [ok] migration on new DB");
}

{
  // 既存 DB に冪等適用 (2 回呼んでもエラーなし)
  const dbPath = path.join(TMP_DIR, "existing.db");
  const db = new Database(dbPath);
  const { applyFlowMigrations } = await import("../../src/flow/db/migrations.js");
  applyFlowMigrations(db);
  // 2 回目 — エラーが発生しないことを確認
  applyFlowMigrations(db);
  db.close();
  console.log("  [ok] migration idempotent");
}

{
  // 想定外の SQL 失敗を migration 済みにしない
  const dbPath = path.join(TMP_DIR, "broken.db");
  const db = new Database(dbPath);
  db.exec("CREATE VIEW discussion_paper AS SELECT 1 AS id");
  const { applyFlowMigrations } = await import("../../src/flow/db/migrations.js");
  assert.throws(() => applyFlowMigrations(db), /discussion_paper|view|table/i, "unexpected migration errors throw");
  const recorded = db
    .prepare("SELECT id FROM _flow_migrations WHERE id = 'flow_0001_initial'")
    .get() as { id: string } | undefined;
  assert.equal(recorded, undefined, "failed migration is not recorded");
  db.close();
  console.log("  [ok] migration unexpected errors are not swallowed");
}

// ── withCostLog ──────────────────────────────────────────────────

{
  const dbPath = path.join(TMP_DIR, "costlog.db");
  process.env.DATABASE_PATH = dbPath;

  const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
  _resetFlowDb();

  const { withCostLog } = await import("../../src/flow/cost-logger.js");
  const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");

  const mock = new MockLLMClient([
    () => ({ ok: true as const, text: "hello", usage: { input_tokens: 10, output_tokens: 5 } }),
  ]);

  const wrapped = withCostLog(mock, {
    flow: "discussion",
    sessionId: "sess-001",
    round: 1,
    turn: 2,
    role: "賛成派",
    persona: "persona-A",
    location: "utterance",
  });

  const result = await wrapped.invoke({ prompt: "test prompt", model: "claude-haiku-4-5-20251001" });
  assert.ok(result.ok, "wrapped client returns ok");
  assert.equal(result.ok && result.text, "hello");

  // DB に記録されているか確認
  const db = new Database(dbPath);
  const row = db
    .prepare("SELECT * FROM llm_call_log ORDER BY id DESC LIMIT 1")
    .get() as Record<string, unknown> | undefined;

  assert.ok(row, "llm_call_log row inserted");
  assert.equal(row!.flow, "discussion");
  assert.equal(row!.session_id, "sess-001");
  assert.equal(row!.round, 1);
  assert.equal(row!.turn, 2);
  assert.equal(row!.role, "賛成派");
  assert.equal(row!.persona, "persona-A");
  assert.equal(row!.location, "utterance");
  assert.equal(row!.input_tokens, 10);
  assert.equal(row!.output_tokens, 5);
  assert.equal(row!.prompt, "test prompt");
  assert.equal(row!.response, "hello");
  assert.ok(typeof row!.latency_ms === "number" && row!.latency_ms >= 0, "latency_ms recorded");
  db.close();
  console.log("  [ok] withCostLog records to llm_call_log");

  // usage なし backend (null 許容)
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "costlog2.db");
  const mockNoUsage = new MockLLMClient([
    () => ({ ok: true as const, text: "no-usage" }),
  ]);
  const wrapped2 = withCostLog(mockNoUsage, {
    flow: "sparring",
    sessionId: "sess-002",
    location: "facilitator",
  });
  await wrapped2.invoke({ prompt: "p" });

  const db2 = new Database(process.env.DATABASE_PATH);
  const row2 = db2
    .prepare("SELECT * FROM llm_call_log ORDER BY id DESC LIMIT 1")
    .get() as Record<string, unknown> | undefined;
  assert.ok(row2, "row inserted for no-usage backend");
  assert.equal(row2!.input_tokens, null, "input_tokens null when usage missing");
  assert.equal(row2!.output_tokens, null, "output_tokens null when usage missing");
  db2.close();
  console.log("  [ok] withCostLog handles missing usage (null)");

  // backend 列: ctx.backend 指定時は記録される
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "costlog-backend.db");
  const mockBackend = new MockLLMClient([
    () => ({ ok: true as const, text: "ok" }),
  ]);
  const wrappedBackend = withCostLog(mockBackend, {
    flow: "discussion",
    sessionId: "sess-backend",
    location: "utterance",
    backend: "anthropic",
  });
  await wrappedBackend.invoke({ prompt: "p" });

  const dbBackend = new Database(process.env.DATABASE_PATH);
  const rowBackend = dbBackend
    .prepare("SELECT * FROM llm_call_log ORDER BY id DESC LIMIT 1")
    .get() as Record<string, unknown> | undefined;
  dbBackend.close();
  assert.ok(rowBackend, "row inserted with backend");
  assert.equal(rowBackend!.backend, "anthropic", "backend recorded from ctx");
  console.log("  [ok] withCostLog records backend from CostLogContext");

  // backend 列: ctx.backend 未指定時は getConfig().llm.backend が使われる
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "costlog-backend-default.db");
  process.env.LLM_BACKEND = "mock";
  const { _resetConfig: _resetConfigForBackend } = await import("../../src/config.js");
  _resetConfigForBackend();
  const mockBackend2 = new MockLLMClient([
    () => ({ ok: true as const, text: "ok" }),
  ]);
  const wrappedBackend2 = withCostLog(mockBackend2, {
    flow: "discussion",
    sessionId: "sess-backend-default",
    location: "utterance",
    // backend を指定しない → getConfig().llm.backend で解決
  });
  await wrappedBackend2.invoke({ prompt: "p" });

  const dbBackend2 = new Database(process.env.DATABASE_PATH);
  const rowBackend2 = dbBackend2
    .prepare("SELECT * FROM llm_call_log ORDER BY id DESC LIMIT 1")
    .get() as Record<string, unknown> | undefined;
  dbBackend2.close();
  assert.ok(rowBackend2, "row inserted without explicit backend");
  assert.equal(rowBackend2!.backend, "mock", "backend resolved from config when ctx.backend omitted");
  console.log("  [ok] withCostLog resolves backend from getConfig() when ctx.backend omitted");

  delete process.env.LLM_BACKEND;
  _resetConfigForBackend();

  // クリーンアップ
  delete process.env.DATABASE_PATH;
  _resetFlowDb();
}
