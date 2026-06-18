/**
 * コスト relay テスト。
 * - costFeedRows: (session,model,backend) 別 + activeSince (累積を保つ)
 * - pushCostFeed: エンドポイント/ボディ整形 + 非ok/例外
 * - relayCostOnce: 空→pushed0 / 行あり→1回POST
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

const TMP_DIR = path.resolve(".tmp/flow-relay-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });

// llm_call_log を seed するヘルパ (withCostLog 経由で実 DB 書込)。
async function seed(dbPath: string): Promise<void> {
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

  // S1: 2 コール (claude-cli / opus), S2: 1 コール (anthropic / haiku)
  await withCostLog(mk(0.01), { flow: "discussion", sessionId: "S1", location: "u", backend: "claude-cli" }).invoke({ prompt: "p", model: "opus" });
  await withCostLog(mk(0.02), { flow: "discussion", sessionId: "S1", location: "u", backend: "claude-cli" }).invoke({ prompt: "p", model: "opus" });
  await withCostLog(mk(0.005), { flow: "improvement", sessionId: "S2", location: "f", backend: "anthropic" }).invoke({ prompt: "p", model: "haiku" });
}

// ── costFeedRows ────────────────────────────────────────────────

{
  const dbPath = path.join(TMP_DIR, "rows.db");
  await seed(dbPath);
  const { costFeedRows } = await import("../../src/flow/cost-report.js");
  const db = new Database(dbPath);

  const rows = costFeedRows(db);
  // S1 は (opus,claude-cli) で 1 行に集約、S2 は (haiku,anthropic)
  const s1 = rows.find((r) => r.sessionId === "S1")!;
  assert.ok(s1, "S1 row present");
  assert.equal(s1.model, "opus");
  assert.equal(s1.backend, "claude-cli");
  assert.equal(s1.calls, 2, "S1 calls aggregated");
  assert.ok(Math.abs(s1.costUsd - 0.03) < 1e-9, "S1 cost cumulative");
  assert.equal(s1.cacheReadTokens, 200, "S1 cache read summed");

  const s2 = rows.find((r) => r.sessionId === "S2")!;
  assert.equal(s2.calls, 1);
  db.close();
  console.log("  [ok] costFeedRows groups by (session,model,backend) with cumulative sums");
}

// ── activeSince は「最近活動したセッションの累積」を返す ──────────

{
  const dbPath = path.join(TMP_DIR, "active.db");
  await seed(dbPath);
  const { costFeedRows } = await import("../../src/flow/cost-report.js");
  const db = new Database(dbPath);

  // 未来の activeSince → 該当セッション無し → 空
  assert.equal(costFeedRows(db, { activeSince: Date.now() + 1_000_000 }).length, 0, "future activeSince → empty");

  // 過去の activeSince → 全セッションが対象、各セッションは累積のまま
  const all = costFeedRows(db, { activeSince: 0 });
  const s1 = all.find((r) => r.sessionId === "S1")!;
  assert.equal(s1.calls, 2, "activeSince keeps cumulative (not delta)");
  db.close();
  console.log("  [ok] costFeedRows activeSince selects active sessions, keeps cumulative");

  delete process.env.DATABASE_PATH;
  const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
  _resetFlowDb();
}

// ── pushCostFeed ────────────────────────────────────────────────

{
  const { pushCostFeed } = await import("../../src/flow/cost-relay.js");

  let captured: { url: string; body: unknown } | null = null;
  const fakeFetch = (url: string, init: { body?: unknown }) => {
    captured = { url, body: JSON.parse(String(init.body)) };
    return Promise.resolve(new Response(JSON.stringify({ ok: true, recorded: 1 }), { status: 200 }));
  };

  const rows = [
    { sessionId: "S1", model: "opus", backend: "claude-cli", calls: 2, inputTokens: 20, outputTokens: 8, cacheReadTokens: 200, cacheCreationTokens: 40, costUsd: 0.03 },
  ];
  const res = await pushCostFeed(
    { baseUrl: "http://anatomia:4200/", path: "/api/cost-feed", label: "anatomia" },
    "discutere", rows, 12345, fakeFetch as unknown as typeof fetch,
  );
  assert.ok(res.ok, "push ok");
  assert.equal(res.recorded, 1);
  assert.equal(captured!.url, "http://anatomia:4200/api/cost-feed", "trailing slash trimmed + path");
  const body = captured!.body as { service: string; ts: number; sessions: unknown[] };
  assert.equal(body.service, "discutere");
  assert.equal(body.ts, 12345);
  assert.equal(body.sessions.length, 1);
  console.log("  [ok] pushCostFeed builds endpoint + body");

  // Concordia は別パス
  const conc = await pushCostFeed(
    { baseUrl: "http://concordia:17330", path: "/v1/cost-feed", label: "concordia" },
    "discutere", rows, 1, fakeFetch as unknown as typeof fetch,
  );
  assert.ok(conc.ok);
  assert.equal(captured!.url, "http://concordia:17330/v1/cost-feed", "concordia path");
  console.log("  [ok] pushCostFeed uses per-target path");

  const bad500 = { baseUrl: "http://x", path: "/api/cost-feed" };
  // 非 ok レスポンス
  const bad = await pushCostFeed(bad500, "s", rows, 1, (() => Promise.resolve(new Response("", { status: 500 }))) as unknown as typeof fetch);
  assert.ok(!bad.ok && /http 500/.test(bad.error!), "non-ok → error");

  // 例外
  const threw = await pushCostFeed(bad500, "s", rows, 1, (() => Promise.reject(new Error("boom"))) as unknown as typeof fetch);
  assert.ok(!threw.ok && /boom/.test(threw.error!), "throw → ok:false");
  console.log("  [ok] pushCostFeed folds non-ok and exceptions into ok:false");
}

// ── buildCostFeedTargets ────────────────────────────────────────

{
  const { buildCostFeedTargets } = await import("../../src/flow/cost-relay.js");
  assert.equal(buildCostFeedTargets({}).length, 0, "no urls → no targets");
  const both = buildCostFeedTargets({ anatomiaUrl: "http://a", concordiaUrl: "http://c" });
  assert.equal(both.length, 2, "both urls → 2 targets");
  assert.equal(both[0].path, "/api/cost-feed");
  assert.equal(both[1].path, "/v1/cost-feed");
  const onlyConc = buildCostFeedTargets({ concordiaUrl: "http://c" });
  assert.equal(onlyConc.length, 1, "only concordia → 1 target");
  assert.equal(onlyConc[0].label, "concordia");
  console.log("  [ok] buildCostFeedTargets maps configured urls to per-service targets");
}

// ── relayCostOnce ───────────────────────────────────────────────

{
  const dbPath = path.join(TMP_DIR, "relay.db");
  await seed(dbPath);
  const { relayCostOnce } = await import("../../src/flow/cost-relay.js");
  const db = new Database(dbPath);

  let calls = 0;
  const seen: string[] = [];
  const fakeFetch = (url: string) => { calls++; seen.push(url); return Promise.resolve(new Response(JSON.stringify({ ok: true, recorded: 2 }), { status: 200 })); };

  // 2 target (Anatomia + Concordia) へ同じ集計を送る
  const targets = [
    { baseUrl: "http://a", path: "/api/cost-feed", label: "anatomia" },
    { baseUrl: "http://c", path: "/v1/cost-feed", label: "concordia" },
  ];
  const r = await relayCostOnce(db, { targets, service: "discutere", ts: 1, fetchImpl: fakeFetch as unknown as typeof fetch });
  assert.ok(r.ok && r.pushed === 2, "2 session-rows pushed (S1,S2)");
  assert.equal(calls, 2, "one POST per target (multiplexed)");
  assert.deepEqual(seen.sort(), ["http://a/api/cost-feed", "http://c/v1/cost-feed"], "pushed to both targets");
  assert.equal(r.results.length, 2, "per-target results");

  // 該当無し (未来 activeSince) → POST しない
  const empty = await relayCostOnce(db, { targets, service: "discutere", activeSince: Date.now() + 1_000_000, ts: 1, fetchImpl: fakeFetch as unknown as typeof fetch });
  assert.ok(empty.ok && empty.pushed === 0, "nothing active → pushed 0");
  assert.equal(calls, 2, "no extra POST when empty");

  // target 無し → POST しない
  const noTarget = await relayCostOnce(db, { targets: [], service: "discutere", ts: 1, fetchImpl: fakeFetch as unknown as typeof fetch });
  assert.ok(noTarget.ok && noTarget.pushed === 0, "no targets → pushed 0");
  assert.equal(calls, 2, "no POST when no targets");
  db.close();
  console.log("  [ok] relayCostOnce multiplexes to all targets + skips empty/no-target");

  delete process.env.DATABASE_PATH;
  const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
  _resetFlowDb();
}
