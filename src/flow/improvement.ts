/**
 * 改善フロー (improvement.md / OVERVIEW §11-c)。
 *
 * 議論フローと骨格は同一 (runFlow を再利用)。差分は step 6 の世論決定を
 * 中立投票でなく機械計算 (design_gap への射影) にする点のみ。
 *   - step 1〜5・7〜9 は議論フローと同一 (FlowDirector の骨格そのまま)。
 *   - step 6: 各意見を design_gap 方向へ射影してスコアし、最高スコア = 世論。中立投票はしない。
 *
 * design_gap = 目標 (intended_affect) − 現状 (基礎感情データ, negative 想定)。
 */

import { runFlow, type FlowDirectorDeps, type FlowDirectorResult, type RoundEvaluator } from "./director.js";
import type { FlowTag } from "./tags.js";
import { buildTargetVector, computeDesignGap, loadCurrentVector, negativeBaselineVector } from "./design-gap.js";
import { pickWinner, scoreOpinions } from "./improvement-score.js";
import { textToVector } from "./sentiment-vector.js";
import { getFlowDb } from "./db/connection.js";

export interface ImprovementFlowDeps extends FlowDirectorDeps {
  /**
   * 意見テキスト → 20 次元感情ベクトル。既定は textToVector (辞書ベース)。
   * テストで決定論的なベクトルを注入するために差し替え可能。
   */
  opinionToVector?: (text: string) => number[];
  /**
   * 現状 (起点) ベクトルを明示指定する (テスト用)。
   * 未指定なら data/games の sentiment.json から読み、無ければ negative ベースライン。
   */
  currentVector?: number[];
}

/** improvement_score テーブルへスコアを記録する。 */
function persistScores(
  sessionId: string,
  round: number,
  scored: Array<{ id: string; score: number }>,
  winner: string | null
): void {
  const db = getFlowDb();
  const now = Date.now();
  const stmt = db.prepare(
    `INSERT INTO improvement_score (session_id, round, utterance_id, score, is_winner, created_at)
     VALUES (?, ?, ?, ?, ?, ?)`
  );
  for (const s of scored) {
    stmt.run(sessionId, round, s.id, s.score, s.id === winner ? 1 : 0, now);
  }
}

/**
 * 改善フローを実行する。
 * 世論決定 = design_gap への射影による機械スコア (中立投票は行わない)。
 */
export async function runImprovementFlow(
  theme: string,
  tags: readonly FlowTag[],
  deps: ImprovementFlowDeps
): Promise<FlowDirectorResult> {
  const opinionToVector = deps.opinionToVector ?? textToVector;
  const gamesDir = deps.gamesDir ?? "./data/games";

  // 現状 (起点) ベクトル: 明示指定 > sentiment.json > negative ベースライン。
  const currentVec =
    deps.currentVector ?? loadCurrentVector(theme, gamesDir) ?? negativeBaselineVector();

  // 機械スコア評価器 (step 6 置換)。LLM を使わないためコストログは出さない (improvement.md)。
  const evaluateRound: RoundEvaluator = async (ctx) => {
    const target = buildTargetVector(ctx.mechanics);
    const designGap = computeDesignGap(currentVec, target);

    const scored = scoreOpinions({
      candidates: ctx.candidates.map((c) => ({ id: c.id, text: c.text })),
      currentVec,
      designGap,
      opinionToVector,
    });
    const winner = pickWinner(scored);

    persistScores(ctx.sessionId, ctx.round, scored, winner);

    // tally には機械スコアを載せる (conclusion は winner のみ参照)。
    // kind="scores" で票数でないことを明示し (respec 03, A10)、表示層が「n 票」表記にしない。
    // winnerShare (投票集中度) は票が無いため算出不能 = null (早期収束は従来条件のまま)。
    const tally: Record<string, number> = {};
    for (const s of scored) tally[s.id] = s.score;
    return { kind: "scores", tally, winner, winnerShare: null };
  };

  return runFlow(theme, tags, { ...deps, flow: "improvement", evaluateRound });
}
