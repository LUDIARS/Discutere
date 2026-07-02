/**
 * T4 改善フロー テスト (2026-07-02 respec PR-C 改訂)。
 * - design_gap = 目標 − 現状 (negative 起点 + positive 目標 → positive 方向)
 * - improvement-score: 予測変化ベクトルを design_gap へ射影してスコア (cosine 近さでなく移動量で勝つ)
 * - 統合: runImprovementFlow が LLM 効果予測 (effect-predict) で世論を確定し、
 *   中立投票 (vote) を呼ばない。method='effect-predict' が improvement_score に永続される。
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import Database from "better-sqlite3";

import {
  buildTargetVector,
  computeDesignGap,
  negativeBaselineVector,
} from "../../src/flow/design-gap.js";
import {
  scoreOpinion,
  scoreOpinions,
  pickWinner,
  type MovementCandidate,
} from "../../src/flow/improvement-score.js";
import { cosine, norm, subtract, VECTOR_DIMS } from "../../src/flow/sentiment-vector.js";

const VALENCE_IDX = VECTOR_DIMS.indexOf("emo.valence");
const FUN_IDX = VECTOR_DIMS.indexOf("asp.fun");
const JOY_IDX = VECTOR_DIMS.indexOf("emo.joy");

// ── design_gap: 目標 − 現状 = positive 方向 ──────────────────────────────────
{
  const current = negativeBaselineVector();
  const target = buildTargetVector([
    {
      name: "テストメカニクス",
      description: "",
      intended_valence: "positive",
      intended_aspects: ["fun"],
      intended_emotions: ["joy"],
    },
  ]);

  assert.equal(target[FUN_IDX], 1, "intended_aspects fun → target asp.fun=1");
  assert.equal(target[JOY_IDX], 1, "intended_emotions joy → target emo.joy=1");
  assert.ok(target[VALENCE_IDX] > current[VALENCE_IDX], "positive 目標は現状より valence 高い");

  const gap = computeDesignGap(current, target);
  assert.equal(gap.length, VECTOR_DIMS.length, "gap は 20 次元");
  assert.ok(gap[VALENCE_IDX] > 0, "design_gap valence は positive 方向");
  assert.ok(gap[FUN_IDX] > 0, "design_gap fun は positive 方向");
  assert.ok(gap[JOY_IDX] > 0, "design_gap joy は positive 方向");
  console.log("  [ok] design_gap: 目標 − 現状 が positive 方向ベクトルになる");
}

// ── improvement-score: 射影 (移動量) で評価、cosine 近さでは決まらない ──────────
{
  const current = negativeBaselineVector();
  const target = buildTargetVector([
    { name: "m", description: "", intended_valence: "positive", intended_aspects: ["fun"], intended_emotions: ["joy"] },
  ]);
  const gap = computeDesignGap(current, target);

  // A = 目標そのもの → cosine(A,target)=1 (最も近い)、移動量 = gap、射影 = |gap|
  const aVec = target.slice();
  // B = 現状から gap 方向へ 2 倍移動 → cosine は A より小さいが、移動量 = 2*gap、射影 = 2|gap|
  const bVec = current.map((v, i) => v + 2 * gap[i]);
  // C = 動かない (現状のまま) → 射影 0
  const cVec = current.slice();
  // D = gap と逆方向 → 射影が負
  const dVec = current.map((v, i) => v - gap[i]);

  // scoreOpinion = 旧 lexicon 方式の fallback 経路 (移動量 = opinionVec − currentVec)
  const scoreA = scoreOpinion(aVec, current, gap);
  const scoreB = scoreOpinion(bVec, current, gap);
  const scoreC = scoreOpinion(cVec, current, gap);
  const scoreD = scoreOpinion(dVec, current, gap);

  // cosine では A が target に最も近い (>= B)
  assert.ok(cosine(aVec, target) >= cosine(bVec, target), "A は target に cosine で最も近い");
  // それでも射影 (移動量) では B が勝つ
  assert.ok(scoreB > scoreA, `B (移動量大) が A (cosine 近) より高スコア (B=${scoreB.toFixed(3)} A=${scoreA.toFixed(3)})`);
  assert.ok(Math.abs(scoreC) < 1e-9, "動かない意見の射影は 0");
  assert.ok(scoreD < 0, "逆方向の意見は負スコア");

  // |gap| と 2|gap| の関係を確認 (射影の定義の裏取り)
  assert.ok(Math.abs(scoreA - norm(gap)) < 1e-6, "A の射影 = |gap|");
  assert.ok(Math.abs(scoreB - 2 * norm(gap)) < 1e-6, "B の射影 = 2|gap|");

  // scoreOpinions は「予測変化ベクトル (movement)」を受ける (respec PR-C シグネチャ)
  const candidates: MovementCandidate[] = [
    { id: "a", movement: subtract(aVec, current), method: "effect-predict" },
    { id: "b", movement: subtract(bVec, current), method: "effect-predict" },
    { id: "c", movement: subtract(cVec, current), method: "lexicon-fallback" },
  ];
  const scored = scoreOpinions({ candidates, designGap: gap });
  assert.equal(pickWinner(scored), "b", "世論 = 最高射影スコアの意見 (cosine 近さでない)");
  assert.equal(scored[0].method, "effect-predict", "method がスコア結果に伝搬する");
  assert.equal(scored[2].method, "lexicon-fallback", "fallback の method も伝搬する");
  console.log("  [ok] improvement-score: design_gap 射影で評価 (移動量で勝つ / cosine で決まらない)");
}

// ── 統合: runImprovementFlow は効果予測で世論を確定、中立投票を呼ばない ────────
{
  const TMP_DIR = path.resolve(".tmp/flow-improvement-test");
  fs.rmSync(TMP_DIR, { recursive: true, force: true });
  fs.mkdirSync(TMP_DIR, { recursive: true });
  process.env.DATABASE_PATH = path.join(TMP_DIR, "improvement.db");

  const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
  const { _resetConfig } = await import("../../src/config.js");
  const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");
  const { runImprovementFlow } = await import("../../src/flow/improvement.js");

  _resetFlowDb();
  _resetConfig();
  process.env.DISCUTERE_FLOW_ROUNDS = "1";
  process.env.DISCUTERE_FLOW_TURNS_PER_ROUND = "2";
  process.env.DISCUTERE_FLOW_PERSONA_COUNT = "2";
  process.env.DISCUTERE_FLOW_VOTER_COUNT = "5"; // 投票が走れば 5 行入るはず → 0 行であることを確認

  // 効果予測 (step 6): 意見テキストで応答を出し分ける。
  //   発話A = 熱い文面だが効果は小さい (+0.2)、発話B = 冷静な文面で効果が大きい (+0.8)。
  const effectResponder = (args: { prompt: string }) =>
    args.prompt.includes("発話A")
      ? '{"changes":[{"dim":"emo.valence","delta":0.2,"reason":"軽微な改善"}],"confidence":"high"}'
      : '{"changes":[{"dim":"emo.valence","delta":0.8,"reason":"体験が大きく改善"}],"confidence":"high"}';

  const responses = [
    () => "[]",                           // ペルソナ価値軸生成 (respec 01・空割当)
    () => "ラウンドの議題を提示します。", // facilitator 開幕
    () => "発話A",                        // turn1
    () => "発話B",                        // turn2
    effectResponder,                      // effect-predict (候補1)
    effectResponder,                      // effect-predict (候補2)
    () => "賛否から新視点が出た。",         // summary
    () => '{"found":false,"summary":""}', // 止揚判定 (なし)
    () => '{"summary":"改善の結論"}',      // 結論
  ];
  const mock = new MockLLMClient(responses, "フォールバック");

  // rng: 名前 2 回 → どちらのターンも opinion (idx=1) を選ばせる
  const seq = [0.1, 0.1, 0.9, 0.9];
  let i = 0;
  const rng = () => (i < seq.length ? seq[i++] : 0.5);

  const current = negativeBaselineVector();

  const result = await runImprovementFlow("改善テストテーマ", [], {
    llm: mock,
    rng,
    gamesDir: path.join(TMP_DIR, "no-games"),
    currentVector: current,
  });

  // 候補が 2 件できていること
  const candidates = result.utterances.filter((u) => u.role !== "facilitator" && !u.isError);
  assert.ok(candidates.length >= 2, `候補意見 >= 2 (got ${candidates.length})`);

  const db = new Database(process.env.DATABASE_PATH);

  // 中立投票が呼ばれない → vote テーブルにこのセッションの行が無い
  const voteRows = db.prepare("SELECT COUNT(*) c FROM vote WHERE session_id = ?").get(result.sessionId) as { c: number };
  assert.equal(voteRows.c, 0, "改善フローでは vote テーブルに行が入らない (中立投票なし)");

  // llm_call_log: location='vote' が無く、effect-predict が候補数ぶんあり、flow='improvement'
  const voteCalls = db
    .prepare("SELECT COUNT(*) c FROM llm_call_log WHERE session_id = ? AND location = 'vote'")
    .get(result.sessionId) as { c: number };
  assert.equal(voteCalls.c, 0, "投票用 LLM 呼び出しが無い");
  const effectCalls = db
    .prepare("SELECT COUNT(*) c FROM llm_call_log WHERE session_id = ? AND location = 'effect-predict'")
    .get(result.sessionId) as { c: number };
  assert.equal(effectCalls.c, candidates.length, "効果予測 LLM が候補ごとに 1 回、withCostLog 経由で記録される");
  const flowRow = db
    .prepare("SELECT DISTINCT flow FROM llm_call_log WHERE session_id = ?")
    .all(result.sessionId) as Array<{ flow: string }>;
  assert.ok(flowRow.every((r) => r.flow === "improvement"), "コストログの flow は improvement");

  // discussion_paper の flow が improvement
  const paper = db.prepare("SELECT flow FROM discussion_paper WHERE session_id = ?").get(result.sessionId) as { flow: string };
  assert.equal(paper.flow, "improvement", "discussion_paper.flow = improvement");

  // improvement_score: 世論 (is_winner=1) が最高スコアの意見、かつ効果の大きい B が勝つ。
  // method='effect-predict' + detail_json (changes[]) が永続される。
  const scores = db
    .prepare(
      "SELECT utterance_id, score, is_winner, method, detail_json FROM improvement_score WHERE session_id = ? ORDER BY score DESC"
    )
    .all(result.sessionId) as Array<{
    utterance_id: string;
    score: number;
    is_winner: number;
    method: string;
    detail_json: string | null;
  }>;
  assert.ok(scores.length >= 2, "improvement_score に意見ごとの行がある");
  assert.equal(scores[0].is_winner, 1, "最高スコアの意見が世論 (is_winner=1)");
  assert.ok(scores.every((s) => s.method === "effect-predict"), "全行 method='effect-predict'");
  assert.ok(scores.every((s) => s.detail_json !== null), "detail_json (changes[]) が永続される");
  const detail = JSON.parse(scores[0].detail_json!) as Array<{ dim: string; delta: number }>;
  assert.equal(detail[0].dim, "emo.valence", "detail_json に予測変化次元が入る");
  const winnerUtterance = result.utterances.find((u) => u.id === scores[0].utterance_id);
  assert.equal(winnerUtterance?.text, "発話B", "効果予測 delta が大きい発話B が世論になる");

  db.close();
  console.log("  [ok] runImprovementFlow: 効果予測スコアで世論確定 / vote 不使用 / method 永続");

  delete process.env.DISCUTERE_FLOW_ROUNDS;
  delete process.env.DISCUTERE_FLOW_TURNS_PER_ROUND;
  delete process.env.DISCUTERE_FLOW_PERSONA_COUNT;
  delete process.env.DISCUTERE_FLOW_VOTER_COUNT;
  delete process.env.DATABASE_PATH;
  _resetFlowDb();
}

// ── 補助: subtract の健全性 (移動量の定義) ─────────────────────────────────────
{
  assert.deepEqual(subtract([3, 2, 1], [1, 1, 1]), [2, 1, 0], "subtract は要素差");
}

console.log("improvement (T4) tests: all passed");
