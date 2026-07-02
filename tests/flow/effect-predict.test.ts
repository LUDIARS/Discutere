/**
 * 効果予測 (respec PR-C / 07-improvement-scoring.md) テスト。
 *
 * - extractJsonObject / normalizePrediction / changesToVector の単体
 * - predictEffect: Mock LLM で成功 / LLM 失敗 / パース失敗 → null
 * - 回帰 (カテゴリエラー A3): 「熱くポジティブな文面だが効果が負」の意見が
 *   「冷静な文面で効果が正」の意見より低スコアになる
 * - 統合: LLM 失敗 → 意見単位で lexicon-fallback + method 記録 + 議論完走
 * - loadCurrentVector: alias 展開 (「モンスト」→ モンスターストライク.sentiment.json) と
 *   不在時の warn (無言 fallback の解消)
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import Database from "better-sqlite3";

import {
  changesToVector,
  extractJsonObject,
  normalizePrediction,
  predictEffect,
} from "../../src/flow/effect-predict.js";
import { scoreMovement } from "../../src/flow/improvement-score.js";
import {
  buildTargetVector,
  computeDesignGap,
  loadCurrentVector,
  negativeBaselineVector,
} from "../../src/flow/design-gap.js";
import { DIM, VECTOR_DIMS } from "../../src/flow/sentiment-vector.js";
import type { LLMClient, LLMInvokeArgs, LLMResult } from "../../src/persona-engine/llm/client.js";

const VALENCE_IDX = VECTOR_DIMS.indexOf("emo.valence");
const FUN_IDX = VECTOR_DIMS.indexOf("asp.fun");

// ── extractJsonObject: コードフェンス / 前置きに耐える ───────────────────────
{
  const fenced =
    '```json\n{"changes":[{"dim":"asp.fun","delta":0.4}],"confidence":"high"}\n```';
  const obj = extractJsonObject(fenced);
  assert.ok(obj, "コードフェンス付きでもオブジェクトを取り出せる");
  assert.ok(Array.isArray(obj!.changes), "changes 配列が取れる");

  const prefixed = '予測します。{"changes":[],"confidence":"low"} 以上です。';
  assert.ok(extractJsonObject(prefixed), "前置き/後置きテキストに耐える");

  assert.equal(extractJsonObject("JSON なし"), null, "JSON が無ければ null");
  assert.equal(extractJsonObject("[1,2,3]"), null, "配列は effect 予測の形ではない → null");
  console.log("  [ok] extractJsonObject: フェンス/前置き耐性");
}

// ── normalizePrediction: 未知次元は捨てる / delta クランプ ────────────────────
{
  const warns: string[] = [];
  const p = normalizePrediction(
    {
      changes: [
        { dim: "asp.fun", delta: 1.5, reason: "clamp される" },
        { dim: "asp.unknown_dim", delta: 0.3 },
        { dim: "emo.valence", delta: -2 },
        { delta: 0.1 }, // dim 欠落は無視
      ],
      confidence: "low",
    },
    (m) => warns.push(m)
  );
  assert.ok(p, "changes が配列なら正規化できる");
  assert.equal(p!.changes.length, 2, "未知次元と dim 欠落は捨てる");
  assert.equal(p!.changes[0].delta, 1, "delta は 1 にクランプ");
  assert.equal(p!.changes[1].delta, -1, "delta は -1 にクランプ");
  assert.equal(p!.confidence, "low", "confidence を保持");
  assert.ok(warns.some((w) => w.includes("asp.unknown_dim")), "未知次元は warn される");

  assert.equal(normalizePrediction({ confidence: "high" }), null, "changes 欠落は null (fallback へ)");
  console.log("  [ok] normalizePrediction: 未知次元除去 + クランプ");
}

// ── changesToVector: 言及次元だけ動き、他は 0 ────────────────────────────────
{
  const v = changesToVector([
    { dim: "emo.valence", delta: 0.5 },
    { dim: "asp.fun", delta: -0.3 },
  ]);
  assert.equal(v.length, DIM, "20 次元");
  assert.equal(v[VALENCE_IDX], 0.5, "言及次元に delta が入る");
  assert.equal(v[FUN_IDX], -0.3, "負の delta も入る");
  assert.ok(
    v.every((x, i) => (i === VALENCE_IDX || i === FUN_IDX ? true : x === 0)),
    "言及のない次元は 0 (移動なし)"
  );
  console.log("  [ok] changesToVector: 言及次元のみの移動ベクトル");
}

/** 単発応答の Mock LLM。 */
function mockLlm(respond: (args: LLMInvokeArgs) => LLMResult): LLMClient {
  return { invoke: async (args) => respond(args) };
}

// ── predictEffect: 成功 / 失敗 / パース失敗 ──────────────────────────────────
{
  const ok = await predictEffect({
    opinion: "チュートリアルを整理する",
    theme: "テストテーマ",
    mechanics: [{ name: "ガチャ", description: "ランダム入手" }],
    llm: mockLlm(() => ({
      ok: true,
      text: '{"changes":[{"dim":"asp.tutorial_ux","delta":0.6,"reason":"導線改善"}],"confidence":"high"}',
    })),
  });
  // asp.tutorial_ux が次元に無い場合は捨てられるので valence で確認し直す
  const ok2 = await predictEffect({
    opinion: "チュートリアルを整理する",
    theme: "テストテーマ",
    mechanics: [],
    llm: mockLlm(() => ({
      ok: true,
      text: '{"changes":[{"dim":"emo.valence","delta":0.6}],"confidence":"high"}',
    })),
  });
  assert.ok(ok && ok2, "正常な JSON は EffectPrediction になる");
  assert.equal(ok2!.changes[0].dim, "emo.valence");

  const warns: string[] = [];
  const failed = await predictEffect({
    opinion: "x",
    theme: "t",
    mechanics: [],
    llm: mockLlm(() => ({ ok: false, error: "boom" })),
    warn: (m) => warns.push(m),
  });
  assert.equal(failed, null, "LLM 失敗は null (呼び出し側で lexicon fallback)");

  const unparsable = await predictEffect({
    opinion: "x",
    theme: "t",
    mechanics: [],
    llm: mockLlm(() => ({ ok: true, text: "了解しました。改善が必要だと思います。" })),
    warn: (m) => warns.push(m),
  });
  assert.equal(unparsable, null, "パース失敗も null");
  assert.ok(warns.length >= 2, "失敗はどちらも warn される (握り潰さない)");
  console.log("  [ok] predictEffect: 成功 / LLM 失敗 / パース失敗");
}

// ── 回帰 (A3 カテゴリエラー): 文面の感情 ≠ 採用効果 ──────────────────────────
{
  // 熱くポジティブな文面だが、採用すると体験が悪化する提案 (効果が負)
  const hot = "絶対最高！！ガチャ排出を渋くして課金圧を上げよう！神ゲー確定で最高に楽しい！！";
  // 冷静な文面だが、採用すると体験が改善する提案 (効果が正)
  const calm = "序盤の難易度曲線を緩和し、チュートリアルの導線を整理する。";

  const llm = mockLlm((args) => ({
    ok: true,
    text: args.prompt.includes("課金圧")
      ? '{"changes":[{"dim":"emo.valence","delta":-0.6,"reason":"課金圧で体験悪化"},{"dim":"asp.monetization","delta":-0.7}],"confidence":"high"}'
      : '{"changes":[{"dim":"emo.valence","delta":0.5,"reason":"導線が改善"},{"dim":"asp.difficulty","delta":0.4}],"confidence":"high"}',
  }));

  const current = negativeBaselineVector();
  const target = buildTargetVector([]); // positive 体験目標
  const gap = computeDesignGap(current, target);

  const hotPred = await predictEffect({ opinion: hot, theme: "改善", mechanics: [], llm });
  const calmPred = await predictEffect({ opinion: calm, theme: "改善", mechanics: [], llm });
  assert.ok(hotPred && calmPred);

  const hotScore = scoreMovement(changesToVector(hotPred!.changes), gap);
  const calmScore = scoreMovement(changesToVector(calmPred!.changes), gap);

  assert.ok(
    hotScore < calmScore,
    `熱い文面でも効果が負なら低スコア (hot=${hotScore.toFixed(3)} < calm=${calmScore.toFixed(3)})`
  );
  assert.ok(hotScore < 0, "効果が負の意見は負スコア (文面のポジティブさに引きずられない)");
  assert.ok(calmScore > 0, "冷静な文面でも効果が正なら正スコア");
  console.log("  [ok] 回帰: 文面の感情でなく採用効果でスコアされる (A3 修正)");
}

// ── 統合: LLM 効果予測が全滅しても lexicon-fallback で完走 + method 記録 ──────
{
  const TMP_DIR = path.resolve(".tmp/flow-effect-predict-test");
  fs.rmSync(TMP_DIR, { recursive: true, force: true });
  fs.mkdirSync(TMP_DIR, { recursive: true });
  process.env.DATABASE_PATH = path.join(TMP_DIR, "effect-predict.db");

  const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
  const { _resetConfig } = await import("../../src/config.js");
  const { runImprovementFlow } = await import("../../src/flow/improvement.js");

  _resetFlowDb();
  _resetConfig();
  process.env.DISCUTERE_FLOW_ROUNDS = "1";
  process.env.DISCUTERE_FLOW_TURNS_PER_ROUND = "2";
  process.env.DISCUTERE_FLOW_PERSONA_COUNT = "2";

  // 効果予測プロンプトだけ失敗させる (他の議論進行はスクリプト応答)。
  const scripted = [
    "ラウンドの議題を提示します。",
    "発話A",
    "発話B",
    "賛否から新視点が出た。",
    '{"found":false,"summary":""}',
    '{"summary":"改善の結論"}',
  ];
  let i = 0;
  const llm: LLMClient = {
    async invoke(args) {
      if (args.prompt.includes("変化しそうな次元")) {
        return { ok: false, error: "effect-predict down" }; // 効果予測 LLM 全滅
      }
      return { ok: true, text: scripted[Math.min(i++, scripted.length - 1)] };
    },
  };

  // rng: どちらのターンも opinion を選ばせる
  const seq = [0.1, 0.1, 0.9, 0.9];
  let r = 0;
  const rng = () => (r < seq.length ? seq[r++] : 0.5);

  const current = negativeBaselineVector();
  const target = buildTargetVector([]);
  const gap = computeDesignGap(current, target);
  const aVec = current.map((v, k) => v + gap[k]);
  const bVec = current.map((v, k) => v + 2 * gap[k]); // fallback (lexicon) でも B が勝つ

  const warns: string[] = [];
  const result = await runImprovementFlow("改善テストテーマ", [], {
    llm,
    rng,
    gamesDir: path.join(TMP_DIR, "no-games"),
    currentVector: current,
    opinionToVector: (t) => (t === "発話A" ? aVec : t === "発話B" ? bVec : current.slice()),
    warn: (m) => warns.push(m),
    log: () => {},
  });

  // 議論は完走する (OVERVIEW §10: 全滅でも止めない)
  assert.equal(result.rounds, 1, "効果予測全滅でも議論は完走する");
  assert.ok(result.conclusion.includes("改善の結論"), "結論まで到達する");
  assert.ok(
    warns.some((m) => m.includes("lexicon")),
    "fallback は warn で明示される (無言 degrade しない)"
  );

  const db = new Database(process.env.DATABASE_PATH);
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
  assert.ok(scores.length >= 2, "improvement_score に行がある");
  assert.ok(scores.every((s) => s.method === "lexicon-fallback"), "method='lexicon-fallback' が記録される");
  assert.ok(scores.every((s) => s.detail_json === null), "fallback 行に detail_json は無い");
  assert.equal(scores[0].is_winner, 1, "fallback でも世論は確定する");
  const winner = result.utterances.find((u) => u.id === scores[0].utterance_id);
  assert.equal(winner?.text, "発話B", "lexicon fallback の移動量で B が勝つ");

  // 失敗した効果予測呼び出しも withCostLog に記録される (location='effect-predict')
  const effectCalls = db
    .prepare("SELECT COUNT(*) c FROM llm_call_log WHERE session_id = ? AND location = 'effect-predict'")
    .get(result.sessionId) as { c: number };
  assert.ok(effectCalls.c >= 2, "失敗した効果予測もコストログに残る");

  db.close();
  console.log("  [ok] 統合: 効果予測全滅 → lexicon-fallback + method 記録 + 完走");

  delete process.env.DISCUTERE_FLOW_ROUNDS;
  delete process.env.DISCUTERE_FLOW_TURNS_PER_ROUND;
  delete process.env.DISCUTERE_FLOW_PERSONA_COUNT;
  delete process.env.DATABASE_PATH;
  _resetFlowDb();
}

// ── loadCurrentVector: alias 展開 + 不在時 warn ──────────────────────────────
{
  const TMP_DIR = path.resolve(".tmp/flow-effect-predict-test/games");
  fs.rmSync(TMP_DIR, { recursive: true, force: true });
  fs.mkdirSync(TMP_DIR, { recursive: true });

  // fixture: 正式名のファイル名 + 目印になる game_vector
  const vec = new Array<number>(DIM).fill(0.5);
  vec[VALENCE_IDX] = 0.9;
  fs.writeFileSync(
    path.join(TMP_DIR, "モンスターストライク.sentiment.json"),
    JSON.stringify({ game: "Monster Strike", slug: "monster-strike", game_vector: vec }),
    "utf-8"
  );

  // 略称「モンスト」→ 静的別名辞書で「モンスターストライク」に展開されてファイルに届く
  const byAlias = loadCurrentVector("モンスト", TMP_DIR);
  assert.ok(byAlias, "「モンスト」で モンスターストライク.sentiment.json が引ける (alias 展開)");
  assert.equal(byAlias![VALENCE_IDX], 0.9, "game_vector がそのまま返る");

  // 略称を含む日本語テーマ文でも届く
  const bySentence = loadCurrentVector("モンストの周回が苦痛", TMP_DIR);
  assert.ok(bySentence, "略称入りのテーマ文でも届く");

  // 一致なし → warn + null (無言 fallback の解消)
  const warns: string[] = [];
  const missing = loadCurrentVector("存在しないゲームXYZ", TMP_DIR, { warn: (m) => warns.push(m) });
  assert.equal(missing, null, "一致しなければ null");
  assert.ok(warns.length > 0 && warns[0].includes("見つかりません"), "不在は warn で明示される");

  console.log("  [ok] loadCurrentVector: alias 展開で日本語略称から解決 + 不在 warn");
}

console.log("effect-predict (respec PR-C) tests: all passed");
