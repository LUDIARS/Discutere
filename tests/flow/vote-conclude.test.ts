/**
 * T3 投票 + ラウンドサマリ + 結論 テスト。
 * - 投票: voterCount 人が発話を選ぶ、vote テーブルに記録
 * - ラウンドサマリ: 止揚が検出され discussion_paper_round に追記
 * - 結論: 【収束】プレフィックスで flow_conclusion に保存
 * - 全体統合: runDiscussionFlow で投票・サマリ・結論が含まれる
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import Database from "better-sqlite3";

const TMP_DIR = path.resolve(".tmp/flow-vote-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });

process.env.DATABASE_PATH = path.join(TMP_DIR, "vote.db");
const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();

const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");
const { runRoundVote } = await import("../../src/flow/vote.js");
const { summarizeRound } = await import("../../src/flow/round-summary.js");
const { generateConclusion } = await import("../../src/flow/conclusion.js");
const { runDiscussionFlow, isVoteCandidate } = await import("../../src/flow/director.js");
const { _resetConfig } = await import("../../src/config.js");

// ── 投票候補フィルタ: facilitator 発話は除外 ───────────────────────────────────

{
  const mk = (over: Record<string, unknown>) => ({
    id: "x", sessionId: "s", paperId: "p", round: 1, turn: 1,
    personaId: "pid", personaName: "n", role: "debater", stance: "pro",
    text: "t", isError: false, ...over,
  }) as Parameters<typeof isVoteCandidate>[0];

  assert.equal(isVoteCandidate(mk({ role: "debater" }), 1), true, "debater is candidate");
  assert.equal(isVoteCandidate(mk({ role: "opinion" }), 1), true, "opinion is candidate");
  assert.equal(isVoteCandidate(mk({ role: "facilitator", turn: 0 }), 1), false, "facilitator opening excluded");
  assert.equal(isVoteCandidate(mk({ role: "facilitator" }), 1), false, "facilitator excluded");
  assert.equal(isVoteCandidate(mk({ isError: true }), 1), false, "error utterance excluded");
  assert.equal(isVoteCandidate(mk({ round: 2 }), 1), false, "other round excluded");
  console.log("  [ok] isVoteCandidate: facilitator / error / other-round excluded");
}

// ── 投票 ──────────────────────────────────────────────────────────────────

{
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "vote1.db");

  const utterances = [
    { id: "u-001", personaName: "太郎", text: "楽しかった" },
    { id: "u-002", personaName: "花子", text: "面白くなかった" },
    { id: "u-003", personaName: "次郎", text: "独創的な体験だった" },
  ];

  // 1票目と2票目は u-003 を選ぶ、3票目は u-001 を選ぶ
  const mock = new MockLLMClient([
    () => "u-003",
    () => "u-003",
    () => "u-001",
  ]);

  const result = await runRoundVote({
    theme: "ゲームの面白さ",
    sessionId: "sess-vote-1",
    round: 1,
    voterCount: 3,
    utterances,
    llm: mock,
  });

  assert.equal(result.tally["u-003"], 2, "u-003 got 2 votes");
  assert.equal(result.tally["u-001"], 1, "u-001 got 1 vote");
  assert.equal(result.winner, "u-003", "winner is u-003");

  const db = new Database(process.env.DATABASE_PATH);
  const voteRows = db
    .prepare("SELECT * FROM vote WHERE session_id = ?")
    .all("sess-vote-1") as Array<Record<string, unknown>>;
  db.close();

  assert.equal(voteRows.length, 3, "3 vote rows in DB");
  const u003Votes = voteRows.filter((r) => r.chosen_utterance_id === "u-003");
  assert.equal(u003Votes.length, 2, "2 rows chose u-003");
  console.log("  [ok] runRoundVote: tally + winner + DB records");
}

{
  // 投票失敗時: 記録が null、winner null
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "vote2.db");

  const mock = new MockLLMClient([
    () => ({ ok: false as const, error: "timeout" }),
  ]);

  const result = await runRoundVote({
    theme: "テスト",
    sessionId: "sess-vote-err",
    round: 1,
    voterCount: 1,
    utterances: [{ id: "u-x", personaName: "A", text: "意見" }],
    llm: mock,
    warn: () => {},
  });

  assert.equal(result.winner, null, "winner null on LLM error");
  const db = new Database(process.env.DATABASE_PATH);
  const rows = db
    .prepare("SELECT chosen_utterance_id FROM vote WHERE session_id = ?")
    .all("sess-vote-err") as Array<Record<string, unknown>>;
  db.close();
  assert.equal(rows.length, 1, "1 vote row even on error");
  assert.equal(rows[0].chosen_utterance_id, null, "chosen_utterance_id is null on error");
  console.log("  [ok] runRoundVote: LLM error → null record");
}

// ── ラウンドサマリ + 止揚 ──────────────────────────────────────────────────

{
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "summary1.db");

  // 先にペーパーと paper_round を作る
  const db = new Database(process.env.DATABASE_PATH);
  const { applyFlowMigrations } = await import("../../src/flow/db/migrations.js");
  applyFlowMigrations(db);
  db.prepare(
    "INSERT INTO discussion_paper (id, flow, session_id, theme, tags_json, mechanics_json, supplement, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
  ).run("p-sum-1", "discussion", "sess-sum-1", "テーマ", "[]", "[]", "", Date.now(), Date.now());
  db.prepare(
    "INSERT INTO discussion_paper_round (paper_id, round, summary, aufhebung_json, created_at) VALUES (?, ?, ?, ?, ?)"
  ).run("p-sum-1", 1, "placeholder", "[]", Date.now());
  db.close();

  const utterances = [
    { personaName: "A", text: "賛成派の意見" },
    { personaName: "B", text: "反対派の意見" },
    { personaName: "C", text: "両方の観点を統合した新しい視点" },
  ];

  // 1回目: サマリ、2回目: 止揚あり
  const mock = new MockLLMClient([
    () => "このラウンドでは賛否の対立から新しい視点が生まれた。",
    () => '{"found":true,"summary":"賛否を超えた統合的な視点が出現した"}',
  ]);

  const result = await summarizeRound({
    theme: "テーマ",
    sessionId: "sess-sum-1",
    paperId: "p-sum-1",
    round: 1,
    utterances,
    stockedAufhebung: [],
    llm: mock,
  });

  assert.ok(result.summary.includes("賛否の対立"), "summary from LLM");
  assert.equal(result.newAufhebung.length, 1, "1 new aufhebung");
  assert.ok(result.newAufhebung[0].includes("統合的"), "aufhebung summary");

  // DB の paper_round が更新されている
  const db2 = new Database(process.env.DATABASE_PATH);
  const row = db2
    .prepare("SELECT * FROM discussion_paper_round WHERE paper_id = ? AND round = ?")
    .get("p-sum-1", 1) as Record<string, unknown> | undefined;
  db2.close();
  assert.ok(row, "paper_round row exists");
  assert.ok(row!.summary.toString().includes("賛否の対立"), "summary updated in DB");
  const aufhebungInDb = JSON.parse(row!.aufhebung_json as string) as string[];
  assert.equal(aufhebungInDb.length, 1, "aufhebung saved to DB");
  console.log("  [ok] summarizeRound: summary + aufhebung detected + DB updated");
}

// ── 結論 ─────────────────────────────────────────────────────────────────────

{
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "conclusion1.db");

  const db = new Database(process.env.DATABASE_PATH);
  const { applyFlowMigrations } = await import("../../src/flow/db/migrations.js");
  applyFlowMigrations(db);
  db.close();

  const utterances = [
    { id: "u-c1", sessionId: "sess-c1", paperId: "p-c1", round: 1, turn: 1,
      personaId: "pid1", personaName: "A", role: "debater", stance: "pro", text: "良い意見", isError: false },
    { id: "u-c2", sessionId: "sess-c1", paperId: "p-c1", round: 1, turn: 2,
      personaId: "pid2", personaName: "B", role: "debater", stance: "con", text: "反論意見", isError: false },
  ];

  const mock = new MockLLMClient([
    () => '{"summary":"賛否を超えた統合的な結論に達した"}',
  ]);

  const result = await generateConclusion({
    theme: "テーマ",
    sessionId: "sess-c1",
    paperId: "p-c1",
    allUtterances: utterances,
    allAufhebung: ["止揚1"],
    voteResults: [{ tally: { "u-c1": 2 }, winner: "u-c1" }],
    llm: mock,
  });

  assert.ok(result.concluded, "concluded=true when aufhebung present");
  assert.ok(result.summary.startsWith("【収束】"), "summary has CONVERGE_PREFIX");
  assert.ok(result.summary.includes("統合的"), "summary contains LLM text");

  const db2 = new Database(process.env.DATABASE_PATH);
  const row = db2
    .prepare("SELECT * FROM flow_conclusion WHERE session_id = ?")
    .get("sess-c1") as Record<string, unknown> | undefined;
  db2.close();
  assert.ok(row, "flow_conclusion row created");
  assert.equal(row!.concluded, 1, "concluded=1");
  assert.ok(row!.summary.toString().startsWith("【収束】"), "stored with prefix");
  console.log("  [ok] generateConclusion: 【収束】prefix + flow_conclusion saved");
}

{
  // 止揚・世論なし → 結論なし
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "conclusion2.db");

  const mock = new MockLLMClient([], "フォールバック");

  const result = await generateConclusion({
    theme: "テーマ",
    sessionId: "sess-c2",
    paperId: "p-c2",
    allUtterances: [],
    allAufhebung: [],
    voteResults: [{ tally: {}, winner: null }],
    llm: mock,
  });

  assert.ok(!result.concluded, "concluded=false when no aufhebung/votes");
  assert.equal(result.summary, "(結論なし)", "summary is 結論なし");
  console.log("  [ok] generateConclusion: no aufhebung → 結論なし");
}

// ── 統合テスト: runDiscussionFlow に T3 が含まれる ────────────────────────────

{
  _resetFlowDb();
  _resetConfig();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "integration-t3.db");
  process.env.DISCUTERE_FLOW_ROUNDS = "1";
  process.env.DISCUTERE_FLOW_TURNS_PER_ROUND = "2";
  process.env.DISCUTERE_FLOW_PERSONA_COUNT = "2";
  process.env.DISCUTERE_FLOW_VOTER_COUNT = "1";

  // ペルソナ価値軸 + ファシリ開幕 + ターン2個 + 投票1回 + サマリ + 止揚 + 結論 = 複数の LLM 呼び出し
  const responses = [
    () => "[]",                                             // ペルソナ価値軸生成 (respec 01・空割当)
    () => "このラウンドのテーマについて意見をどうぞ。",        // ファシリテーター開幕ターン
    () => "発話A",                                          // ターン1
    () => "発話B",                                          // ターン2
    () => "u-first-id-in-session",                         // 投票 (IDが解析できなくてもokとする)
    () => "発話AとBの対立から新視点が生まれた。",              // サマリ
    () => '{"found":true,"summary":"統合的な止揚が出た"}',   // 止揚判定
    () => '{"summary":"対立を超えた結論"}',                  // 結論
  ];

  const mock = new MockLLMClient(responses, "フォールバック");

  let seed = 555;
  const rng = () => { seed = (seed * 1664525 + 1013904223) & 0xffffffff; return (seed >>> 0) / 0x100000000; };

  const result = await runDiscussionFlow("統合テストテーマ", [], {
    llm: mock,
    rng,
    gamesDir: path.join(TMP_DIR, "no-games"),
  });

  assert.equal(result.rounds, 1, "1 round");
  assert.ok(result.utterances.length >= 1, `>= 1 utterances (got ${result.utterances.length})`);

  const db = new Database(process.env.DATABASE_PATH);
  const voteRows = db.prepare("SELECT * FROM vote WHERE session_id = ?").all(result.sessionId);
  const conclusionRow = db
    .prepare("SELECT * FROM flow_conclusion WHERE session_id = ?")
    .get(result.sessionId) as Record<string, unknown> | undefined;
  db.close();

  assert.ok(voteRows.length >= 1, `vote rows >= 1 (got ${voteRows.length})`);
  assert.ok(conclusionRow, "flow_conclusion row created");
  console.log("  [ok] runDiscussionFlow integration: vote + conclusion present");

  delete process.env.DISCUTERE_FLOW_ROUNDS;
  delete process.env.DISCUTERE_FLOW_TURNS_PER_ROUND;
  delete process.env.DISCUTERE_FLOW_PERSONA_COUNT;
  delete process.env.DISCUTERE_FLOW_VOTER_COUNT;
}

delete process.env.DATABASE_PATH;
_resetFlowDb();
