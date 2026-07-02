/**
 * respec 02 — シャッフル輪番 + ファシリテーター文脈 テスト。
 * - buildTurnOrder: rng 固定で決定的 / turnsPerRound=人数 で全員 1 回ずつ
 * - buildTurnOrder: turnsPerRound > 人数 でも連続同一発言者が出ない (周回境界 swap)
 * - buildTurnOrder: facilitator が対象から除外される
 * - buildFacilitatorPrompt: round 1 は文脈なし / round 2 以降は前ラウンド summary/止揚/winner を含む
 * - 統合: facilitator の発話は turn 0 のみ、各発言者がラウンド内で 1 回ずつ発言する
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import Database from "better-sqlite3";

const TMP_DIR = path.resolve(".tmp/flow-turn-scheduler-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });
process.env.DATABASE_PATH = path.join(TMP_DIR, "scheduler.db");

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();
const { _resetConfig } = await import("../../src/config.js");
_resetConfig();

const { buildTurnOrder, shuffle } = await import("../../src/flow/turn-scheduler.js");
const { buildFacilitatorPrompt } = await import("../../src/flow/facilitator-turn.js");
const { generateFlowPersonas } = await import("../../src/flow/personas.js");
const { runDiscussionFlow } = await import("../../src/flow/director.js");
const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");

function lcg(seedInit: number) {
  let seed = seedInit;
  return () => {
    seed = (seed * 1664525 + 1013904223) & 0xffffffff;
    return (seed >>> 0) / 0x100000000;
  };
}

// ── buildTurnOrder: 決定的 + 全員 1 回ずつ + facilitator 除外 ──────────────────
{
  const personas = generateFlowPersonas({ count: 5, defaultModel: "m", isLocal: false, rng: lcg(42) });
  const speakers = personas.filter((p) => p.role !== "facilitator");

  const order1 = buildTurnOrder(personas, speakers.length, lcg(7));
  const order2 = buildTurnOrder(personas, speakers.length, lcg(7));
  assert.deepEqual(
    order1.map((p) => p.id),
    order2.map((p) => p.id),
    "rng 固定で turn 順が決定的"
  );

  assert.equal(order1.length, speakers.length, "turnsPerRound=人数 の長さ");
  assert.equal(new Set(order1.map((p) => p.id)).size, speakers.length, "全員がちょうど 1 回ずつ");
  assert.ok(order1.every((p) => p.role !== "facilitator"), "facilitator は輪番対象外");
  console.log("  [ok] buildTurnOrder: deterministic + everyone once + facilitator excluded");
}

// ── buildTurnOrder: turnsPerRound < 人数 → 先頭から重複なく充てる ─────────────
{
  const personas = generateFlowPersonas({ count: 6, defaultModel: "m", isLocal: false, rng: lcg(1) });
  const order = buildTurnOrder(personas, 3, lcg(2));
  assert.equal(order.length, 3, "turnsPerRound=3 の長さ");
  assert.equal(new Set(order.map((p) => p.id)).size, 3, "重複なし");
  console.log("  [ok] buildTurnOrder: turnsPerRound < speakers → no duplicates");
}

// ── buildTurnOrder: 複数周回でも連続同一発言者なし (多 seed で網羅) ────────────
{
  const personas = generateFlowPersonas({ count: 4, defaultModel: "m", isLocal: false, rng: lcg(3) });
  for (let seed = 1; seed <= 50; seed++) {
    const order = buildTurnOrder(personas, 9, lcg(seed)); // 3 speakers × 3 周
    assert.equal(order.length, 9);
    for (let i = 1; i < order.length; i++) {
      assert.notEqual(order[i].id, order[i - 1].id, `seed=${seed}: 連続同一発言者が出ない (i=${i})`);
    }
  }
  console.log("  [ok] buildTurnOrder: no consecutive speaker across lap boundaries (50 seeds)");
}

// ── shuffle: 非破壊 + 要素保存 ────────────────────────────────────────────────
{
  const src = [1, 2, 3, 4, 5];
  const out = shuffle(src, lcg(9));
  assert.deepEqual(src, [1, 2, 3, 4, 5], "入力は破壊しない");
  assert.deepEqual([...out].sort(), [1, 2, 3, 4, 5], "要素は保存される");
  console.log("  [ok] shuffle: non-destructive permutation");
}

// ── buildFacilitatorPrompt: round 1 は文脈なし / round 2 は前ラウンド文脈 ──────
{
  const p1 = buildFacilitatorPrompt("テストテーマ", 1, null);
  assert.ok(p1.includes("ラウンド 1"), "round 1 プロンプト");
  assert.ok(!p1.includes("前ラウンドの結果"), "round 1 に前ラウンド節なし");

  const p2 = buildFacilitatorPrompt("テストテーマ", 2, {
    round: 1,
    summary: "R1 の対立は課金と体験価値だった",
    aufhebung: ["両立の止揚"],
    winner: { personaName: "凛", text: "課金は体験を壊さない設計が前提だと思う。".repeat(10) },
  });
  assert.ok(p2.includes("# 前ラウンドの結果"), "前ラウンド節あり");
  assert.ok(p2.includes("R1 の対立は課金と体験価値だった"), "summary を含む");
  assert.ok(p2.includes("止揚: 両立の止揚"), "止揚を含む");
  assert.ok(p2.includes("世論 (最多得票): 凛:"), "winner の発話主体名を含む");
  assert.ok(!p2.includes("課金は体験を壊さない設計が前提だと思う。".repeat(10)), "winner text は 120 字に切る");
  assert.ok(p2.includes("前ラウンドの繰り返しは避ける"), "指示を含む");

  // winner なし (全員棄権) でも節は出る
  const p3 = buildFacilitatorPrompt("t", 3, { round: 2, summary: "S", aufhebung: [], winner: null });
  assert.ok(p3.includes("# 前ラウンドの結果\nS"), "winner/止揚なしでも summary は含む");
  assert.ok(!p3.includes("世論"), "winner なしなら世論行なし");
  console.log("  [ok] buildFacilitatorPrompt: round-1 plain / round-2+ with previous round context");
}

// ── 統合: facilitator は turn 0 のみ、各発言者がラウンド内 1 回ずつ ─────────────
{
  _resetFlowDb();
  _resetConfig();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "integration.db");

  const mock = new MockLLMClient([], "輪番テストの発話です。");
  const result = await runDiscussionFlow("輪番テーマ", [], {
    llm: mock,
    rng: lcg(555),
    rounds: 2,
    turnsPerRound: 3, // personaCount 既定 4 → facilitator を除く 3 人 = 全員 1 回ずつ
    possess: false,
    gamesDir: path.join(TMP_DIR, "no-games"),
    log: () => {},
    warn: () => {},
  });

  const db = new Database(process.env.DATABASE_PATH);
  const rows = db
    .prepare("SELECT round, turn, persona_id, role FROM flow_utterance WHERE session_id = ? ORDER BY round, turn")
    .all(result.sessionId) as Array<{ round: number; turn: number; persona_id: string; role: string }>;
  db.close();

  const facilitatorRows = rows.filter((r) => r.role === "facilitator");
  assert.ok(facilitatorRows.length >= 2, "facilitator 開幕発話がラウンドごとにある");
  assert.ok(facilitatorRows.every((r) => r.turn === 0), "facilitator は turn 0 のみで発話 (ターンループ不参加)");

  for (const round of [1, 2]) {
    const turnRows = rows.filter((r) => r.round === round && r.turn > 0);
    assert.equal(turnRows.length, 3, `round ${round}: 3 ターン`);
    assert.equal(new Set(turnRows.map((r) => r.persona_id)).size, 3, `round ${round}: 全員 1 回ずつ`);
  }
  console.log("  [ok] integration: facilitator opening-only + round-robin coverage");
}

delete process.env.DATABASE_PATH;
_resetFlowDb();
console.log("turn-scheduler (respec 02) tests: all passed");
