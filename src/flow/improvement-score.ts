/**
 * 改善フローの機械スコアリング (improvement.md (2) / discussion.md step 6 の置換)。
 *
 * 各意見について「その意見を採ったときの感情ベクトルの動き (予測変化ベクトル)」を
 * design_gap 方向へ射影し、positive な改善ベクトル (= design_gap 方向への移動量) が
 * 大きいほど高スコアにする。
 *
 *   score(意見) = projection( movement , design_gap )
 *
 * movement の求め方 (2026-07-02 改訂 — カテゴリエラーの修正):
 *   - 正: LLM 効果予測 (effect-predict.ts の changesToVector) — 「採用した場合の体験変化」
 *   - fallback: 旧 lexicon 方式 (textToVector(意見文) − currentVec) — LLM 失敗時のみ意見単位で
 * どちらを使ったかは method ('effect-predict' / 'lexicon-fallback') で保持し
 * improvement_score.method に永続する (degrade の可視化)。
 *
 * 「近さ (cosine)」ではなく negative→positive の移動量で評価する点が肝
 * (improvement.md 実装メモ)。最高スコアの意見 = そのラウンドの世論 (主要意見)。
 * 改善フローでは中立投票を行わない (機械スコアのみで世論を確定する)。
 */

import { scalarProjection, subtract } from "./sentiment-vector.js";
import type { EffectChange } from "./effect-predict.js";

/** スコアの算出方式 (improvement_score.method 列)。旧データは 'lexicon'。 */
export type ScoreMethod = "effect-predict" | "lexicon-fallback";

/** スコアリング対象 1 意見 (予測変化ベクトルまで解決済み)。 */
export interface MovementCandidate {
  id: string;
  /** 意見を採用した場合の移動 (予測変化) ベクトル。20 次元。 */
  movement: number[];
  method: ScoreMethod;
  /** 効果予測の changes[] (method='effect-predict' のみ)。detail_json 永続用。 */
  detail?: EffectChange[];
}

export interface ScoredOpinion {
  id: string;
  score: number;
  method: ScoreMethod;
  detail?: EffectChange[];
}

/**
 * 移動 (予測変化) ベクトルのスコア = design_gap 方向へ射影した量。
 * 正で大きいほど negative → positive (design_gap 方向) へ動かす良い改善意見。
 */
export function scoreMovement(movement: number[], designGap: number[]): number {
  return scalarProjection(movement, designGap);
}

/**
 * 旧 lexicon 方式の 1 意見スコア (fallback 用)。
 * 移動ベクトル = opinionVec − currentVec を design_gap へ射影する。
 */
export function scoreOpinion(opinionVec: number[], currentVec: number[], designGap: number[]): number {
  return scoreMovement(subtract(opinionVec, currentVec), designGap);
}

/**
 * 候補意見をスコアリングする (射影は不変、入力が「予測変化ベクトル」に変わった)。
 */
export function scoreOpinions(args: {
  candidates: MovementCandidate[];
  designGap: number[];
}): ScoredOpinion[] {
  const { candidates, designGap } = args;
  return candidates.map((c) => ({
    id: c.id,
    score: scoreMovement(c.movement, designGap),
    method: c.method,
    ...(c.detail ? { detail: c.detail } : {}),
  }));
}

/**
 * 最高スコアの意見 id を世論 (主要意見) として返す。同点は先着 (candidates 順)。
 * 候補が無ければ null。
 */
export function pickWinner(scored: ScoredOpinion[]): string | null {
  let winner: string | null = null;
  let max = -Infinity;
  for (const s of scored) {
    if (s.score > max) {
      max = s.score;
      winner = s.id;
    }
  }
  return winner;
}
