/**
 * T6 壁打ちフロー テスト。
 * - ユーザ発話起点で応答が返り、次のユーザ発話まで待てる (不定期)
 * - flow.sparringMaxTurns 到達で安全に打ち切る
 * - 「まとめて」でラウンドが締まり続行可能、「おわり」でフロー終了 + 結論まとめ
 * - コマンドのゆらぎ (「まとめてください」) を拾う
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";

import { detectSparringCommand } from "../../src/flow/sparring-commands.js";

// ── コマンド検出 (文字列一致 + ゆらぎ) ──────────────────────────────────────
{
  assert.equal(detectSparringCommand("まとめて"), "summarize", "まとめて");
  assert.equal(detectSparringCommand("まとめてください"), "summarize", "ゆらぎ: まとめてください");
  assert.equal(detectSparringCommand("そろそろまとめて。"), "summarize", "句点/前置きつき");
  assert.equal(detectSparringCommand("おわり"), "end", "おわり");
  assert.equal(detectSparringCommand("終わりにして"), "end", "ゆらぎ: 終わりにして");
  assert.equal(detectSparringCommand("もう終了で"), "end", "終了");
  assert.equal(
    detectSparringCommand("このメカニクスはまとめて評価すると面白いが、難易度が高すぎて序盤で飽きると思う"),
    "none",
    "長い意見文中の「まとめて」はコマンドにしない"
  );
  assert.equal(detectSparringCommand("まだ終わらないでほしい"), "none", "否定形は終了にしない");
  assert.equal(detectSparringCommand("コンボが楽しい"), "none", "通常発話");
  console.log("  [ok] detectSparringCommand: 文字列一致 + ゆらぎ + 誤検知抑制");
}

// ── セッション動作 ──────────────────────────────────────────────────────────
{
  const TMP = path.resolve(".tmp/flow-sparring-test");
  fs.rmSync(TMP, { recursive: true, force: true });
  fs.mkdirSync(TMP, { recursive: true });
  process.env.DATABASE_PATH = path.join(TMP, "sparring.db");

  const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
  const { _resetConfig } = await import("../../src/config.js");
  const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");
  const { SparringSession } = await import("../../src/flow/sparring.js");
  const Database = (await import("better-sqlite3")).default;

  _resetFlowDb();
  _resetConfig();
  process.env.DISCUTERE_FLOW_PERSONA_COUNT = "2";
  process.env.DISCUTERE_FLOW_SPARRING_MAX_TURNS = "3"; // 暴走ガードを低く設定

  // 応答・サマリ・止揚・結論をひととおり返す mock (順序は呼ばれ次第)
  const mock = new MockLLMClient(
    [
      () => "なるほど、その点は重要ですね。",       // 応答1
      () => "別の見方もありますよ。",                // 応答2
      () => "ラウンドの要約です。",                  // まとめて: summary
      () => '{"found":false,"summary":""}',          // まとめて: 止揚判定
      () => "さらに掘り下げましょう。",              // 応答3
      () => "ここまでの要約。",                      // おわり: summary
      () => '{"found":true,"summary":"統合した視点"}', // おわり: 止揚あり → 結論材料
      () => '{"summary":"壁打ちの結論"}',            // おわり: 結論
    ],
    "フォールバック応答"
  );

  let seed = 99;
  const rng = () => {
    seed = (seed * 1664525 + 1013904223) & 0xffffffff;
    return (seed >>> 0) / 0x100000000;
  };

  const session = new SparringSession("壁打ちテーマ", [], {
    llm: mock,
    rng,
    responsesPerTurn: 1,
    gamesDir: path.join(TMP, "no-games"),
  });
  await session.start();

  // 1) ユーザ発話 → 応答が返る
  const r1 = await session.submitUser("最初の意見です");
  assert.equal(r1.kind, "responses", "通常発話 → responses");
  if (r1.kind === "responses") {
    assert.equal(r1.user.role, "user", "ユーザ発話が記録される");
    assert.ok(r1.utterances.length >= 1, "AI 応答が 1 件以上返る");
    assert.equal(r1.utterances[0].isError, false, "応答はエラーでない");
  }

  // 2) 不定期に次のユーザ発話 → また応答 (待てる)
  const r2 = await session.submitUser("二つ目の意見");
  assert.equal(r2.kind, "responses", "次のユーザ発話にも応答");

  // 3) 「まとめてください」(ゆらぎ) → ラウンド締め、続行可能
  const r3 = await session.submitUser("まとめてください");
  assert.equal(r3.kind, "round-summary", "まとめて → round-summary");
  if (r3.kind === "round-summary") assert.equal(r3.round, 1, "ラウンド1が締まる");
  assert.equal(session.isEnded, false, "まとめて後も継続可能");

  // 4) まだ続けられる (新ラウンド)
  const r4 = await session.submitUser("続きの意見");
  assert.equal(r4.kind, "responses", "まとめて後も発話を受け付ける");

  // 5) 「おわり」→ フロー終了 + 結論
  const r5 = await session.submitUser("おわり");
  assert.equal(r5.kind, "ended", "おわり → ended");
  if (r5.kind === "ended") {
    assert.equal(r5.reason, "command", "終了理由 = command");
    assert.ok(r5.concluded, "結論が出る");
    assert.ok(r5.conclusion.includes("結論"), "結論まとめが返る");
  }
  assert.equal(session.isEnded, true, "セッション終了");

  // 終了後の発話は ended を返す
  const r6 = await session.submitUser("まだ話したい");
  assert.equal(r6.kind, "ended", "終了後は ended");

  const db = new Database(process.env.DATABASE_PATH);
  const flowRows = db
    .prepare("SELECT DISTINCT flow FROM llm_call_log WHERE session_id = ?")
    .all(session.sessionId) as Array<{ flow: string }>;
  assert.ok(flowRows.every((r) => r.flow === "sparring"), "コストログ flow=sparring");
  // 投票は行わない
  const voteRows = db.prepare("SELECT COUNT(*) c FROM vote WHERE session_id = ?").get(session.sessionId) as { c: number };
  assert.equal(voteRows.c, 0, "壁打ちは投票しない");
  db.close();
  console.log("  [ok] SparringSession: ユーザ起点応答 / まとめて / おわり / 投票なし");

  // ── 暴走ガード: sparringMaxTurns 到達で打ち切り ──────────────────────────
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP, "sparring-guard.db");
  process.env.DISCUTERE_FLOW_SPARRING_MAX_TURNS = "2";
  _resetConfig();

  const guardMock = new MockLLMClient(
    [
      () => "応答1",
      () => "応答2",
      // 2 ターン到達後の finalize 用 (summary/止揚/結論)
      () => "最終要約",
      () => '{"found":false,"summary":""}',
      () => '{"summary":"打ち切り結論"}',
    ],
    "フォールバック"
  );
  let s2 = 7;
  const rng2 = () => {
    s2 = (s2 * 1664525 + 1013904223) & 0xffffffff;
    return (s2 >>> 0) / 0x100000000;
  };
  const guarded = new SparringSession("ガードテーマ", [], {
    llm: guardMock,
    rng: rng2,
    responsesPerTurn: 1,
    gamesDir: path.join(TMP, "no-games"),
  });
  await guarded.start();

  await guarded.submitUser("1");
  await guarded.submitUser("2");
  // 3 回目: aiTurnCount(=2) >= max(2) → ガードで打ち切り
  const guardResult = await guarded.submitUser("3");
  assert.equal(guardResult.kind, "ended", "上限到達で ended");
  if (guardResult.kind === "ended") assert.equal(guardResult.reason, "guard", "理由 = guard");
  assert.equal(guarded.isEnded, true, "ガードでセッション終了");
  console.log("  [ok] SparringSession: sparringMaxTurns 到達で安全に打ち切り");

  delete process.env.DISCUTERE_FLOW_PERSONA_COUNT;
  delete process.env.DISCUTERE_FLOW_SPARRING_MAX_TURNS;
  delete process.env.DATABASE_PATH;
  _resetFlowDb();
}

console.log("sparring (T6) tests: all passed");
