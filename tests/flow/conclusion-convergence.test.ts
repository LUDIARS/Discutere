/**
 * respec 04 — 結論入力拡充 + concluded 判定統一 + 収束シグナル テスト。
 * - converge プロンプトに全ラウンドサマリ (# 各ラウンドの要約) が含まれる
 * - Mock LLM が壊れた JSON を 2 回返す → concluded=false + 【収束(未検証)】 + warn
 * - 1 回目失敗 → 2 回目成功 → concluded=true (リトライ動作)
 * - 止揚 JSON 失敗時に warn が出る (round-summary)
 * - 早期収束: 止揚件数到達 + winnerShare 0.4 では打ち切られない / 0.75 なら打ち切られる
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import Database from "better-sqlite3";
import type { LLMClient, LLMInvokeArgs, LLMResult } from "../../src/persona-engine/llm/client.js";

const TMP_DIR = path.resolve(".tmp/flow-conclusion-convergence-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });
process.env.DATABASE_PATH = path.join(TMP_DIR, "conclusion.db");

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();
const { _resetConfig } = await import("../../src/config.js");
_resetConfig();

const { generateConclusion, buildRoundsSection, UNVERIFIED_PREFIX } = await import("../../src/flow/conclusion.js");
const { summarizeRound } = await import("../../src/flow/round-summary.js");
const { runDiscussionFlow } = await import("../../src/flow/director.js");
const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");

function lcg(seedInit: number) {
  let seed = seedInit;
  return () => {
    seed = (seed * 1664525 + 1013904223) & 0xffffffff;
    return (seed >>> 0) / 0x100000000;
  };
}

const baseUtterances = [
  { id: "u-c1", sessionId: "s", paperId: "p", round: 1, turn: 1,
    personaId: "pid1", personaName: "A", role: "debater", stance: "pro", text: "良い意見", isError: false },
];

// ── converge プロンプトに全ラウンドサマリが含まれる ────────────────────────────
{
  const rounds = [
    { round: 1, summary: "R1サマリ: 課金の是非で対立", aufhebung: ["R1止揚"] },
    { round: 2, summary: "R2サマリ: 体験価値の議論に進展", aufhebung: [] },
  ];
  const section = buildRoundsSection(rounds);
  assert.ok(section.startsWith("# 各ラウンドの要約"), "節見出し");
  assert.ok(section.includes("ラウンド 1: R1サマリ: 課金の是非で対立"), "R1 サマリ");
  assert.ok(section.includes("止揚: R1止揚"), "R1 止揚");
  assert.ok(section.includes("ラウンド 2: R2サマリ: 体験価値の議論に進展"), "R2 サマリ");
  assert.equal(buildRoundsSection([]), "", "ラウンドなしは空");

  const prompts: string[] = [];
  const capture: LLMClient = {
    async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
      prompts.push(args.prompt);
      return { ok: true, text: '{"summary":"結論本文"}' };
    },
  };
  const result = await generateConclusion({
    theme: "テーマ",
    sessionId: "sess-rounds-1",
    paperId: "p-rounds-1",
    allUtterances: baseUtterances,
    allAufhebung: ["止揚1"],
    rounds,
    voteResults: [{ tally: { "u-c1": 2 }, winner: "u-c1" }],
    llm: capture,
    warn: () => {},
  });
  assert.ok(result.concluded, "concluded=true");
  assert.equal(prompts.length, 1, "LLM 1 回");
  assert.ok(prompts[0].includes("# 各ラウンドの要約"), "converge プロンプトにラウンド要約節");
  assert.ok(prompts[0].includes("R1サマリ: 課金の是非で対立"), "R1 が入力に含まれる");
  assert.ok(prompts[0].includes("R2サマリ: 体験価値の議論に進展"), "R2 が入力に含まれる");
  console.log("  [ok] generateConclusion: all round summaries included in converge prompt");
}

// ── 壊れた JSON ×2 → concluded=false + 【収束(未検証)】 + warn ─────────────────
{
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "broken.db");

  const warns: string[] = [];
  const mock = new MockLLMClient([
    () => "壊れた応答テキスト (JSONなし) その1",
    () => "壊れた応答テキスト (JSONなし) その2",
  ]);

  const result = await generateConclusion({
    theme: "テーマ",
    sessionId: "sess-broken",
    paperId: "p-broken",
    allUtterances: baseUtterances,
    allAufhebung: ["止揚1"],
    rounds: [],
    voteResults: [],
    llm: mock,
    warn: (m) => warns.push(m),
  });

  assert.equal(mock.calls, 2, "1 回リトライ (計 2 回呼ぶ)");
  assert.equal(result.concluded, false, "2 回失敗で concluded=false");
  assert.ok(result.summary.startsWith(UNVERIFIED_PREFIX), `summary は ${UNVERIFIED_PREFIX} 始まり`);
  assert.ok(result.summary.includes("その2"), "最後の生テキストを保持");
  assert.ok(warns.some((w) => w.includes("リトライ")), "リトライの warn");
  assert.ok(warns.some((w) => w.includes("2 回失敗")), "degrade 明示の warn");

  const db = new Database(process.env.DATABASE_PATH);
  const row = db
    .prepare("SELECT summary, concluded FROM flow_conclusion WHERE session_id = ?")
    .get("sess-broken") as { summary: string; concluded: number };
  db.close();
  assert.equal(row.concluded, 0, "DB でも concluded=0");
  assert.ok(row.summary.startsWith(UNVERIFIED_PREFIX), "DB summary も未検証プレフィックス");
  console.log("  [ok] generateConclusion: broken JSON x2 → concluded=false + 未検証 prefix + warn");
}

// ── 1 回目失敗 → 2 回目成功 → concluded=true (リトライ動作) ────────────────────
{
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "retry.db");

  const warns: string[] = [];
  const mock = new MockLLMClient([
    () => "1 回目は壊れた応答",
    () => '{"summary":"リトライで得た結論"}',
  ]);

  const result = await generateConclusion({
    theme: "テーマ",
    sessionId: "sess-retry",
    paperId: "p-retry",
    allUtterances: baseUtterances,
    allAufhebung: ["止揚1"],
    voteResults: [],
    llm: mock,
    warn: (m) => warns.push(m),
  });

  assert.equal(mock.calls, 2, "リトライで 2 回呼ぶ");
  assert.equal(result.concluded, true, "リトライ成功で concluded=true");
  assert.ok(result.summary.startsWith("【収束】"), "通常の収束プレフィックス");
  assert.ok(result.summary.includes("リトライで得た結論"), "リトライの結論本文");
  assert.ok(warns.some((w) => w.includes("リトライ")), "リトライした事実は warn に残る");
  console.log("  [ok] generateConclusion: fail → retry success → concluded=true");
}

// ── 止揚 JSON パース失敗の warn (round-summary) ───────────────────────────────
{
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "aufhebung-warn.db");
  const db = new Database(process.env.DATABASE_PATH);
  const { applyFlowMigrations } = await import("../../src/flow/db/migrations.js");
  applyFlowMigrations(db);
  db.close();

  const warns: string[] = [];
  const mock = new MockLLMClient([
    () => "ラウンドのサマリです。",
    () => "止揚判定のはずが JSON でない応答",
  ]);

  const result = await summarizeRound({
    theme: "テーマ",
    sessionId: "sess-auf-warn",
    paperId: "p-auf-warn",
    round: 1,
    utterances: [
      { personaName: "A", text: "賛成" },
      { personaName: "B", text: "反対" },
    ],
    stockedAufhebung: [],
    llm: mock,
    warn: (m) => warns.push(m),
  });

  assert.equal(result.newAufhebung.length, 0, "止揚なし扱いは維持");
  assert.ok(warns.some((w) => w.includes("JSON パース失敗")), "止揚 JSON 失敗の warn が出る");
  console.log("  [ok] summarizeRound: aufhebung JSON parse failure warns (not silent)");
}

// ── 早期収束: 止揚到達 + winnerShare の AND 条件 ──────────────────────────────
// personaCount=4 → 発言者 3 人 (turnsPerRound=3 で全員発言)。
// voteSchedule の発話テキストへ投票し、winnerShare を制御する。
function smartLLM(voteSchedule: string[]): { llm: LLMClient; calls: () => number } {
  let turnCount = 0;
  let voteIdx = 0;
  let calls = 0;
  const llm: LLMClient = {
    async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
      calls += 1;
      const p = args.prompt;
      if (p.includes("ペルソナ一覧")) return { ok: true, text: "[]" };
      if (p.includes("中立な審判として")) {
        const target = voteSchedule[voteIdx % voteSchedule.length];
        voteIdx += 1;
        const m = p.match(new RegExp(`\\[ID:([^\\]]+)\\] ${target}(?:\\n|$)`));
        return { ok: true, text: m ? m[1] : "棄権します" };
      }
      if (p.includes("アウフヘーベン")) return { ok: true, text: '{"found":true,"summary":"止揚S"}' };
      if (p.includes("締めのまとめ")) return { ok: true, text: '{"summary":"収束の結論"}' };
      if (p.includes("要約してください")) return { ok: true, text: "ラウンドの要約" };
      if (p.includes("議論の進行役です")) return { ok: true, text: "議題を提示します。" };
      turnCount += 1;
      return { ok: true, text: `発話${turnCount}` };
    },
  };
  return { llm, calls: () => calls };
}

async function runConvergenceCase(dbName: string, voterCount: number, voteSchedule: string[]) {
  _resetFlowDb();
  _resetConfig();
  process.env.DATABASE_PATH = path.join(TMP_DIR, dbName);
  process.env.DISCUTERE_FLOW_PERSONA_COUNT = "4";
  process.env.DISCUTERE_FLOW_VOTER_COUNT = String(voterCount);
  process.env.DISCUTERE_FACILITATOR_AUFHEBUNG_TARGET = "1"; // 1 ラウンド目で止揚到達
  process.env.DISCUTERE_FLOW_CONVERGE_SHARE = "0.6";

  const { llm } = smartLLM(voteSchedule);
  const result = await runDiscussionFlow("早期収束テーマ", [], {
    llm,
    rng: lcg(99),
    rounds: 2,
    turnsPerRound: 3,
    possess: false,
    gamesDir: path.join(TMP_DIR, "no-games"),
    log: () => {},
    warn: () => {},
  });

  delete process.env.DISCUTERE_FLOW_PERSONA_COUNT;
  delete process.env.DISCUTERE_FLOW_VOTER_COUNT;
  delete process.env.DISCUTERE_FACILITATOR_AUFHEBUNG_TARGET;
  delete process.env.DISCUTERE_FLOW_CONVERGE_SHARE;
  return result;
}

{
  // winnerShare = 2/5 = 0.4 < 0.6 → 止揚が到達しても打ち切らない (2 ラウンド完走)
  const split = await runConvergenceCase("share-low.db", 5, ["発話1", "発話1", "発話2", "発話2", "発話3"]);
  assert.equal(split.rounds, 2, `世論が割れている (share 0.4) 間は完走する (got ${split.rounds})`);

  // winnerShare = 3/4 = 0.75 ≥ 0.6 → 止揚到達 + 集中で 1 ラウンドで早期収束
  const focused = await runConvergenceCase("share-high.db", 4, ["発話1", "発話1", "発話1", "発話2"]);
  assert.equal(focused.rounds, 1, `止揚到達 + share 0.75 で早期収束 (got ${focused.rounds})`);
  console.log("  [ok] early convergence: aufhebung target AND winnerShare (0.4 continues / 0.75 stops)");
}

delete process.env.DATABASE_PATH;
_resetFlowDb();
_resetConfig();
console.log("conclusion-convergence (respec 04) tests: all passed");
