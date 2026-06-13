/**
 * T7 起動経路 テスト。
 * - parseFlowKind: ラベル → FlowKind (議論/改善/学習/壁打ち)、未指定/不正は null
 * - dispatchFlow: フロー選択に応じて正しいドライバを起動 / flow 必須 (不正は受理しない)
 * - parseForumEntry / handleForumFlowPost: フォーラム適用タグ → (flow, tags)、flow なしは拒否
 * - Web ルート: テーマ + 議論タイプ + タグ で起動、状態ポーリング
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";

import { parseFlowKind, dispatchFlow, FlowTypeRequiredError } from "../../src/flow/dispatch.js";
import { parseForumEntry, handleForumFlowPost } from "../../src/flow/entry-discord.js";

// ── parseFlowKind ─────────────────────────────────────────────────────────────
{
  assert.equal(parseFlowKind("議論"), "discussion");
  assert.equal(parseFlowKind("discussion"), "discussion");
  assert.equal(parseFlowKind("改善"), "improvement");
  assert.equal(parseFlowKind("改善提案"), "improvement");
  assert.equal(parseFlowKind("学習"), "learning");
  assert.equal(parseFlowKind("壁打ち"), "sparring");
  assert.equal(parseFlowKind("sparring"), "sparring");
  assert.equal(parseFlowKind(""), null, "空は null");
  assert.equal(parseFlowKind(undefined), null, "undefined は null");
  assert.equal(parseFlowKind("雑談"), null, "未知ラベルは null");
  console.log("  [ok] parseFlowKind: ラベル → FlowKind / 未指定は null");
}

// ── dispatchFlow: フロー選択 → 正しいドライバ (注入スパイで検証) ─────────────
{
  const calls: string[] = [];
  const fakeDirectorResult = {
    sessionId: "s",
    paperId: "p",
    utterances: [],
    rounds: 1,
    conclusion: "x",
    concluded: true,
  };
  const fakeLearning = { gameSlug: "g", opinionsRecorded: 0, mechanicsRecorded: 0, polarityBreakdown: {} };
  const drivers = {
    discussion: async () => {
      calls.push("discussion");
      return fakeDirectorResult;
    },
    improvement: async () => {
      calls.push("improvement");
      return fakeDirectorResult;
    },
    learning: async () => {
      calls.push("learning");
      return fakeLearning;
    },
  };
  const baseDeps = { llm: { invoke: async () => ({ ok: true as const, text: "" }) }, core: {} as never, drivers };

  await dispatchFlow({ theme: "t", tags: [], flow: "議論" }, baseDeps);
  await dispatchFlow({ theme: "t", tags: [], flow: "改善" }, baseDeps);
  await dispatchFlow({ theme: "t", tags: [], flow: "学習" }, baseDeps);
  assert.deepEqual(calls, ["discussion", "improvement", "learning"], "ラベルごとに正しいドライバ");

  // 壁打ちは SparringSession を返す (start 済み) — LLM は呼ばれない (start は調査のみ)
  const spar = await dispatchFlow(
    { theme: "壁打ちテーマ", tags: [], flow: "壁打ち" },
    { llm: { invoke: async () => ({ ok: true as const, text: "" }) }, gamesDir: path.resolve(".tmp/none-xyz") }
  );
  assert.equal(spar.kind, "sparring", "壁打ち → sparring");
  if (spar.kind === "sparring") assert.ok(typeof spar.session.submitUser === "function", "SparringSession を返す");

  // flow 必須: 不正/未指定は受理しない
  await assert.rejects(
    () => dispatchFlow({ theme: "t", tags: [], flow: "" }, baseDeps),
    FlowTypeRequiredError,
    "flow 未指定は FlowTypeRequiredError"
  );
  await assert.rejects(() => dispatchFlow({ theme: "t", tags: [], flow: "雑談" }, baseDeps), FlowTypeRequiredError);
  console.log("  [ok] dispatchFlow: フロー選択 → 正しいドライバ / flow 必須");
}

// ── parseForumEntry: フォーラム適用タグ → (flow, tags) ──────────────────────
{
  assert.deepEqual(parseForumEntry(["改善", "機密"]), { flow: "improvement", tags: ["機密"] }, "改善 + 機密");
  assert.deepEqual(parseForumEntry(["壁打ち", "運用"]), { flow: "sparring", tags: ["運用"] }, "壁打ち + 運用");
  assert.deepEqual(parseForumEntry(["議論"]), { flow: "discussion", tags: [] }, "議論のみ");
  // 議論タイプタグが無い → flow null (受理しない方針)
  assert.deepEqual(parseForumEntry(["面白さ"]).flow, null, "旧方向タグのみは flow null");
  assert.deepEqual(parseForumEntry([]).flow, null, "タグなしは flow null");
  console.log("  [ok] parseForumEntry: 適用タグ → flow + tags");
}

// ── handleForumFlowPost: flow なしは拒否 / あれば dispatch ───────────────────
{
  const calls: string[] = [];
  const drivers = {
    improvement: async () => {
      calls.push("improvement");
      return { sessionId: "s", paperId: "p", utterances: [], rounds: 1, conclusion: "", concluded: false };
    },
  };
  const deps = { llm: { invoke: async () => ({ ok: true as const, text: "" }) }, drivers };

  const rejected = await handleForumFlowPost({ theme: "テーマ", appliedTagNames: ["面白さ"] }, deps);
  assert.equal(rejected.ok, false, "議論タイプタグなしは受理しない");
  if (!rejected.ok) assert.equal(rejected.reason, "flow-required");

  const accepted = await handleForumFlowPost({ theme: "テーマ", appliedTagNames: ["改善", "内部"] }, deps);
  assert.equal(accepted.ok, true, "改善タグで受理");
  assert.deepEqual(calls, ["improvement"], "improvement ドライバが呼ばれる");
  console.log("  [ok] handleForumFlowPost: flow 必須 / 適用タグから dispatch");
}

// ── Web ルート (Hono app.request) ───────────────────────────────────────────
{
  const TMP = path.resolve(".tmp/flow-entrypoints-web");
  fs.rmSync(TMP, { recursive: true, force: true });
  fs.mkdirSync(TMP, { recursive: true });
  process.env.DATABASE_PATH = path.join(TMP, "flow.db");

  const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
  const { _resetConfig } = await import("../../src/config.js");
  const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");
  const { Hono } = await import("hono");
  const { flowRoutes, setFlowWebDeps, _resetFlowWeb } = await import("../../src/flow/web/routes.js");

  _resetFlowDb();
  _resetConfig();
  _resetFlowWeb();
  process.env.DISCUTERE_FLOW_PERSONA_COUNT = "2";
  process.env.DISCUTERE_FLOW_SPARRING_MAX_TURNS = "10";

  const mock = new MockLLMClient([], "AI 応答です");
  setFlowWebDeps({ workspaceId: "knowledge", llm: mock, gamesDir: path.join(TMP, "no-games") });

  const app = new Hono();
  app.route("/", flowRoutes);

  const post = (url: string, body: unknown) =>
    app.request(url, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(body) });

  // 議論タイプ未指定 → 400
  const r400 = await post("/api/flow/start", { theme: "テーマ" });
  assert.equal(r400.status, 400, "flow 未指定は 400");

  // テーマ未指定 → 400
  const rNoTheme = await post("/api/flow/start", { theme: "", flow: "discussion" });
  assert.equal(rNoTheme.status, 400, "テーマ未指定は 400");

  // 壁打ち起動 → sessionId、say → 応答、status → 発話取得
  const rStart = await post("/api/flow/start", { theme: "壁打ちテーマ", flow: "壁打ち", tags: [] });
  const startBody = (await rStart.json()) as { ok: boolean; kind: string; sessionId: string };
  assert.equal(startBody.ok, true, "壁打ち start 成功");
  assert.equal(startBody.kind, "sparring");
  assert.ok(startBody.sessionId, "sessionId 返却");

  const rSay = await post(`/api/flow/${startBody.sessionId}/say`, { text: "最初の意見" });
  assert.equal(rSay.status, 200, "say 成功");

  const rStatus = await app.request(`/api/flow/${startBody.sessionId}/status?since=0`);
  const statusBody = (await rStatus.json()) as { ok: boolean; utterances: Array<{ role: string }> };
  assert.equal(statusBody.ok, true, "status 取得");
  assert.ok(statusBody.utterances.length >= 1, "ユーザ + AI 発話が status に出る");
  assert.ok(statusBody.utterances.some((u) => u.role === "user"), "ユーザ発話含む");

  // say 先が存在しない session → 404
  const r404 = await post("/api/flow/nope/say", { text: "x" });
  assert.equal(r404.status, 404, "未知 session の say は 404");

  // GET /flow は HTML
  const rPage = await app.request("/flow");
  assert.equal(rPage.status, 200, "/flow は 200");
  assert.ok((await rPage.text()).includes("議論タイプ"), "UI に議論タイプ選択がある");

  console.log("  [ok] flow web routes: start(必須検証) / 壁打ち say / status / 404 / UI");

  delete process.env.DISCUTERE_FLOW_PERSONA_COUNT;
  delete process.env.DISCUTERE_FLOW_SPARRING_MAX_TURNS;
  delete process.env.DATABASE_PATH;
  _resetFlowDb();
}

console.log("entrypoints (T7) tests: all passed");
