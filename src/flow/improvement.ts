/**
 * 改善フロー (improvement.md / OVERVIEW §11-c)。
 *
 * 議論フローと骨格は同一 (runFlow を再利用)。差分は step 6 の世論決定を
 * 中立投票でなく機械計算 (design_gap への射影) にする点のみ。
 *   - step 1〜5・7〜9 は議論フローと同一 (FlowDirector の骨格そのまま)。
 *   - step 6: 各意見の「採用した場合の体験変化」を LLM で予測 (effect-predict) し、
 *     予測変化ベクトルを design_gap 方向へ射影してスコア。最高スコア = 世論。中立投票はしない。
 *
 * design_gap = 目標 (intended_affect) − 現状 (基礎感情データ, negative 想定)。
 *
 * 2026-07-02 改訂 (respec PR-C / 指摘 A3): 旧 `textToVector(意見文)` は文面の感情であって
 * 採用効果ではない (カテゴリエラー)。LLM 効果予測を正とし、LLM 失敗時のみ意見単位で
 * 旧 lexicon 方式へフォールバックする (method 列に記録 + warn、議論は完走する)。
 */

import { runFlow, type FlowDirectorDeps, type FlowDirectorResult, type RoundEvaluator } from "./director.js";
import type { FlowTag } from "./tags.js";
import { getConfig } from "../config.js";
import { buildTargetVector, computeDesignGap, loadCurrentVector, negativeBaselineVector } from "./design-gap.js";
import { pickWinner, scoreOpinions, type MovementCandidate, type ScoredOpinion } from "./improvement-score.js";
import { changesToVector, predictEffect } from "./effect-predict.js";
import { subtract, textToVector } from "./sentiment-vector.js";
import { withCostLog } from "./cost-logger.js";
import { getFlowDb } from "./db/connection.js";

export interface ImprovementFlowDeps extends FlowDirectorDeps {
  /**
   * 意見テキスト → 20 次元感情ベクトル (旧 lexicon 方式)。**LLM 効果予測が失敗した意見の
   * fallback にだけ**使う。既定は textToVector (辞書ベース)。テストで差し替え可能。
   */
  opinionToVector?: (text: string) => number[];
  /**
   * 現状 (起点) ベクトルを明示指定する (テスト用)。
   * 未指定なら data/games の sentiment.json から読み (game-aliases で別名展開)、
   * 無ければ warn の上 negative ベースライン。
   */
  currentVector?: number[];
}

/** improvement_score テーブルへスコアを記録する (method / detail_json 込み)。 */
function persistScores(
  sessionId: string,
  round: number,
  scored: ScoredOpinion[],
  winner: string | null
): void {
  const db = getFlowDb();
  const now = Date.now();
  const stmt = db.prepare(
    `INSERT INTO improvement_score (session_id, round, utterance_id, score, is_winner, method, detail_json, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
  );
  for (const s of scored) {
    stmt.run(
      sessionId,
      round,
      s.id,
      s.score,
      s.id === winner ? 1 : 0,
      s.method,
      s.detail ? JSON.stringify(s.detail) : null,
      now
    );
  }
}

/**
 * 改善フローを実行する。
 * 世論決定 = LLM 効果予測 → design_gap への射影による機械スコア (中立投票は行わない)。
 */
export async function runImprovementFlow(
  theme: string,
  tags: readonly FlowTag[],
  deps: ImprovementFlowDeps
): Promise<FlowDirectorResult> {
  const cfg = getConfig();
  const opinionToVector = deps.opinionToVector ?? textToVector;
  const gamesDir = deps.gamesDir ?? "./data/games";
  const warn = deps.warn ?? ((m) => console.warn(`[flow/warn] ${m}`));
  const effectModel = cfg.flow.improvement.effectModel;

  // 現状 (起点) ベクトル: 明示指定 > sentiment.json (別名展開) > negative ベースライン。
  // ベースラインへの fallback は無言にしない (respec 07 §4)。
  let currentVec = deps.currentVector ?? loadCurrentVector(theme, gamesDir, { warn });
  if (!currentVec) {
    warn(
      `テーマ「${theme}」の現状ベクトルが解決できないため negative ベースラインへフォールバックします`
    );
    currentVec = negativeBaselineVector();
  }
  const resolvedCurrentVec = currentVec;

  // 機械スコア評価器 (step 6 置換)。効果予測 LLM は withCostLog 経由 (location="effect-predict")。
  const evaluateRound: RoundEvaluator = async (ctx) => {
    const target = buildTargetVector(ctx.mechanics);
    const designGap = computeDesignGap(resolvedCurrentVec, target);

    const effectLlm = withCostLog(ctx.llm, {
      flow: "improvement",
      sessionId: ctx.sessionId,
      round: ctx.round,
      role: "evaluator",
      location: "effect-predict",
    });

    // 各意見の移動ベクトルを解決する。正 = LLM 効果予測 / 失敗時のみ旧 lexicon 方式 (意見単位)。
    const movements: MovementCandidate[] = [];
    for (const c of ctx.candidates) {
      const prediction = await predictEffect({
        opinion: c.text,
        theme: ctx.theme,
        mechanics: ctx.mechanics,
        llm: effectLlm,
        ...(effectModel ? { model: effectModel } : {}),
        warn: ctx.warn,
      });
      if (prediction) {
        movements.push({
          id: c.id,
          movement: changesToVector(prediction.changes),
          method: "effect-predict",
          detail: prediction.changes,
        });
      } else {
        ctx.warn(
          `効果予測に失敗したため意見 ${c.id} (${c.personaName}) は旧 lexicon 方式で採点します (method=lexicon-fallback)`
        );
        movements.push({
          id: c.id,
          movement: subtract(opinionToVector(c.text), resolvedCurrentVec),
          method: "lexicon-fallback",
        });
      }
    }

    const scored = scoreOpinions({ candidates: movements, designGap });
    const winner = pickWinner(scored);

    persistScores(ctx.sessionId, ctx.round, scored, winner);

    // tally には機械スコアを載せる (conclusion は winner のみ参照)。
    const tally: Record<string, number> = {};
    for (const s of scored) tally[s.id] = s.score;
    return { tally, winner };
  };

  return runFlow(theme, tags, { ...deps, flow: "improvement", evaluateRound });
}
