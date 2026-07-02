/**
 * respec 03 — レンズ投票 + 候補シャッフル + 番号受理 + tally 意味分離 テスト。
 * - voterCount=3 で 3 投票者のプロンプトが相互に異なる (レンズ行 + 候補順)
 * - parseVoteId("2", ...) が提示順 2 番目の候補 ID に解決される
 * - 同点: rng 固定で winner が決定的、かつ提示順 (挿入順) に依存しない
 * - improvement 経路の onVote が kind:"scores" を受け、表示行が票数表記にならない
 * - winnerShare = maxVotes / 有効票数
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import type { LLMInvokeArgs, LLMResult } from "../../src/persona-engine/llm/client.js";

const TMP_DIR = path.resolve(".tmp/flow-vote-lenses-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });
process.env.DATABASE_PATH = path.join(TMP_DIR, "vote-lenses.db");

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();
const { _resetConfig } = await import("../../src/config.js");
_resetConfig();

const { runRoundVote, parseVoteId, pickVoteWinner } = await import("../../src/flow/vote.js");
const { DEFAULT_VOTE_LENSES } = await import("../../src/flow/vote-lenses.js");
const { buildVoteRankingLines, voteNoticeTitle, shouldStackVoteReactions, hasVisibleVoteResult } =
  await import("../../src/flow/vote-display.js");
const { negativeBaselineVector, buildTargetVector, computeDesignGap } = await import("../../src/flow/design-gap.js");
const { runImprovementFlow } = await import("../../src/flow/improvement.js");
const { loadConfig } = await import("../../src/config.js");
const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");

function lcg(seedInit: number) {
  let seed = seedInit;
  return () => {
    seed = (seed * 1664525 + 1013904223) & 0xffffffff;
    return (seed >>> 0) / 0x100000000;
  };
}

const utterances = [
  { id: "u-aaa", personaName: "太郎", text: "楽しかった" },
  { id: "u-bbb", personaName: "花子", text: "面白くなかった" },
  { id: "u-ccc", personaName: "次郎", text: "独創的な体験だった" },
];

// ── voterCount=3: 3 投票者のプロンプトが相互に異なる (レンズ + 候補順) ──────────
{
  const prompts: string[] = [];
  const capture = {
    async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
      prompts.push(args.prompt);
      return { ok: true, text: "u-aaa" };
    },
  };

  const result = await runRoundVote({
    theme: "テーマ",
    sessionId: "sess-lens-1",
    round: 1,
    voterCount: 3,
    utterances,
    llm: capture,
    rng: lcg(123),
    warn: () => {},
  });

  assert.equal(prompts.length, 3, "3 投票");
  assert.equal(new Set(prompts).size, 3, "3 投票者のプロンプトが相互に異なる");
  // レンズ行 (巡回割当)
  assert.ok(prompts[0].includes(DEFAULT_VOTE_LENSES[0].instruction), "voter0 = logic レンズ");
  assert.ok(prompts[1].includes(DEFAULT_VOTE_LENSES[1].instruction), "voter1 = grounds レンズ");
  assert.ok(prompts[2].includes(DEFAULT_VOTE_LENSES[2].instruction), "voter2 = relevance レンズ");
  assert.ok(prompts.every((p) => p.includes("中立な審判として")), "中立審判の前置きは維持");
  assert.equal(result.tally["u-aaa"], 3, "全票 u-aaa");
  assert.equal(result.kind, "votes", "kind=votes");
  assert.equal(result.winnerShare, 1, "winnerShare = 3/3");
  console.log("  [ok] runRoundVote: 3 voters get distinct prompts (lens + order)");
}

// ── 番号回答の受理: 提示順に解決 ──────────────────────────────────────────────
{
  const validIds = new Set(utterances.map((u) => u.id));
  const presented = ["u-ccc", "u-aaa", "u-bbb"]; // この投票者に提示した順
  assert.equal(parseVoteId("2", validIds, presented), "u-aaa", "「2」→ 提示順 2 番目");
  assert.equal(parseVoteId("2.", validIds, presented), "u-aaa", "「2.」も受理");
  assert.equal(parseVoteId("2 番", validIds, presented), "u-aaa", "「2 番」も受理");
  assert.equal(parseVoteId("1", validIds, presented), "u-ccc", "「1」→ 提示順 1 番目");
  assert.equal(parseVoteId("9", validIds, presented), null, "範囲外の番号は棄権");
  assert.equal(parseVoteId("u-bbb", validIds, presented), "u-bbb", "ID 完全一致は従来通り");
  assert.equal(parseVoteId("選んだのは u-ccc です", validIds, presented), "u-ccc", "部分一致は従来通り");
  assert.equal(parseVoteId("2 番目の意見が良い", validIds, presented), null, "番号 + 長文はID優先のため棄権 (誤爆防止)");
  console.log("  [ok] parseVoteId: numeric answers resolve to presented order");
}

// ── 投票経路の統合: LLM が番号で答えても提示順の候補に票が入る ──────────────────
{
  const chosen: string[] = [];
  const numberVoter = {
    async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
      // 提示 1 番目の候補に番号で投票する
      const m = args.prompt.match(/1\. \[ID:([^\]]+)\]/);
      chosen.push(m ? m[1] : "?");
      return { ok: true, text: "1" };
    },
  };
  const result = await runRoundVote({
    theme: "テーマ",
    sessionId: "sess-lens-2",
    round: 1,
    voterCount: 3,
    utterances,
    llm: numberVoter,
    rng: lcg(9),
    warn: () => {},
  });
  const total = Object.values(result.tally).reduce((s, n) => s + n, 0);
  assert.equal(total, 3, "番号回答が棄権にならず 3 票とも有効");
  for (const id of chosen) {
    assert.ok(result.tally[id] >= 1, `提示 1 番目 (${id}) に票が入る`);
  }
  console.log("  [ok] runRoundVote: numeric replies counted against per-voter presented order");
}

// ── 同点くじ: rng 固定で決定的 + 挿入順 (提示順) に依存しない ────────────────────
{
  const tallyAB: Record<string, number> = { "u-aaa": 2, "u-bbb": 2, "u-ccc": 1 };
  const tallyBA: Record<string, number> = { "u-ccc": 1, "u-bbb": 2, "u-aaa": 2 }; // 挿入順を逆に

  const w1 = pickVoteWinner(tallyAB, lcg(42));
  const w2 = pickVoteWinner(tallyAB, lcg(42));
  const w3 = pickVoteWinner(tallyBA, lcg(42));
  assert.equal(w1.winner, w2.winner, "rng 固定で決定的");
  assert.equal(w1.winner, w3.winner, "挿入順に依存しない (id ソート後にくじ)");
  assert.ok(w1.winner === "u-aaa" || w1.winner === "u-bbb", "同点候補から選ばれる");
  assert.equal(w1.maxVotes, 2, "maxVotes=2");
  assert.equal(pickVoteWinner({}, lcg(1)).winner, null, "空 tally は winner なし");
  console.log("  [ok] pickVoteWinner: deterministic tie lottery, order-independent");
}

// ── vote-display: scores は票数表記にしない ───────────────────────────────────
{
  const scoresEvent = {
    kind: "scores" as const,
    tally: { "u-aaa": 1.2345, "u-bbb": -0.5 },
    winner: "u-aaa",
  };
  const lines = buildVoteRankingLines(scoresEvent, (id) => (id === "u-aaa" ? "太郎" : "花子"));
  assert.ok(lines[0].includes("スコア 1.23 (design_gap 適合)"), "scores はスコア表記");
  assert.ok(lines.every((l) => !l.includes("票")), "scores に「票」表記が出ない");
  assert.ok(lines[0].startsWith("🏆"), "winner に 🏆");
  assert.equal(shouldStackVoteReactions("scores"), false, "scores は 👍 の枚数積みをしない");
  assert.equal(shouldStackVoteReactions("votes"), true, "votes は従来通り");
  assert.ok(voteNoticeTitle(2, "scores").includes("機械スコア"), "scores のタイトルは機械スコア");
  assert.ok(voteNoticeTitle(2, "votes").includes("投票結果"), "votes のタイトルは投票結果");
  assert.ok(hasVisibleVoteResult(scoresEvent), "scores は winner があれば可視化");
  assert.ok(!hasVisibleVoteResult({ kind: "scores", tally: {}, winner: null }), "winner なし scores は非表示");

  const votesEvent = { kind: "votes" as const, tally: { "u-aaa": 2 }, winner: "u-aaa" };
  const vLines = buildVoteRankingLines(votesEvent, () => "太郎");
  assert.ok(vLines[0].includes("2票"), "votes は票数表記のまま");
  console.log("  [ok] vote-display: scores vs votes formatting");
}

// ── improvement 統合: onVote が kind:"scores" を受ける ────────────────────────
{
  _resetFlowDb();
  _resetConfig();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "improvement.db");
  process.env.DISCUTERE_FLOW_PERSONA_COUNT = "2";

  const responses = [
    () => "[]",                           // ペルソナ価値軸生成
    () => "改善の議題を提示します。",       // facilitator 開幕
    () => "発話A",                        // turn1
    () => "発話B",                        // turn2
    () => "要約です。",                    // summary
    () => '{"found":false,"summary":""}', // 止揚判定
    () => '{"summary":"結論"}',            // 結論
  ];
  const mock = new MockLLMClient(responses, "フォールバック");

  const current = negativeBaselineVector();
  const target = buildTargetVector([]);
  const gap = computeDesignGap(current, target);
  const aVec = current.map((v, k) => v + gap[k]);
  const bVec = current.map((v, k) => v + 2 * gap[k]);

  const voteEvents: Array<{ kind: string; tally: Record<string, number>; winner: string | null }> = [];
  const result = await runImprovementFlow("改善テーマ", [], {
    llm: mock,
    rng: lcg(7),
    rounds: 1,
    turnsPerRound: 2,
    possess: false,
    gamesDir: path.join(TMP_DIR, "no-games"),
    currentVector: current,
    opinionToVector: (t) => (t === "発話A" ? aVec : t === "発話B" ? bVec : current.slice()),
    onVote: (e) => {
      voteEvents.push({ kind: e.kind, tally: e.tally, winner: e.winner });
    },
    log: () => {},
    warn: () => {},
  });

  assert.ok(result.sessionId, "session 生成");
  assert.equal(voteEvents.length, 1, "onVote 1 回");
  assert.equal(voteEvents[0].kind, "scores", "improvement の onVote は kind=scores");
  const lines = buildVoteRankingLines(
    { kind: "scores", tally: voteEvents[0].tally, winner: voteEvents[0].winner },
    () => "名前"
  );
  assert.ok(lines.every((l) => !l.includes("票")), "improvement の表示行に「票」が出ない");
  console.log("  [ok] runImprovementFlow: onVote receives kind=scores, no vote-count wording");

  delete process.env.DISCUTERE_FLOW_PERSONA_COUNT;
}

// ── config: flow.voteLenses の既定 + env JSON 上書き ──────────────────────────
{
  _resetConfig();
  const before = process.env.DISCUTERE_FLOW_VOTE_LENSES;
  delete process.env.DISCUTERE_FLOW_VOTE_LENSES;
  const cfg = loadConfig();
  assert.equal(cfg.flow.voteLenses.length, 3, "既定レンズ 3 種");
  assert.deepEqual(cfg.flow.voteLenses.map((l) => l.key), ["logic", "grounds", "relevance"]);
  assert.equal(cfg.flow.convergeShare, 0.6, "convergeShare 既定 0.6");
  assert.equal(cfg.flow.personaSetupModel, "", "personaSetupModel 既定は空 (メインモデル)");

  process.env.DISCUTERE_FLOW_VOTE_LENSES = JSON.stringify([{ key: "cost", instruction: "コスト効率を最重視して選ぶ" }]);
  const cfg2 = loadConfig();
  assert.equal(cfg2.flow.voteLenses.length, 1, "env JSON で上書き");
  assert.equal(cfg2.flow.voteLenses[0].key, "cost");
  if (before === undefined) delete process.env.DISCUTERE_FLOW_VOTE_LENSES;
  else process.env.DISCUTERE_FLOW_VOTE_LENSES = before;
  console.log("  [ok] config: flow.voteLenses default + env override, convergeShare/personaSetupModel");
}

delete process.env.DATABASE_PATH;
_resetFlowDb();
_resetConfig();
console.log("vote-lenses (respec 03) tests: all passed");
