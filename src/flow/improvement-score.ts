/**
 * 改善フローの機械スコアリング (improvement.md (2) / discussion.md step 6 の置換)。
 *
 * 各意見について「その意見を採ったときの感情ベクトルの動き」を design_gap 方向へ射影し、
 * positive な改善ベクトル (= design_gap 方向への移動量) が大きいほど高スコアにする。
 *
 *   score(意見) = projection( opinionVec − currentVec , design_gap )
 *
 * 「近さ (cosine)」ではなく negative→positive の移動量で評価する点が肝
 * (improvement.md 実装メモ)。最高スコアの意見 = そのラウンドの世論 (主要意見)。
 * 改善フローでは中立投票を行わない (機械スコアのみで世論を確定する)。
 */

import { scalarProjection, subtract } from "./sentiment-vector.js";

export interface OpinionCandidate {
  id: string;
  text: string;
}

export interface ScoredOpinion {
  id: string;
  score: number;
}

/**
 * 1 意見のスコア = 意見の移動ベクトル (opinionVec − currentVec) を design_gap 方向へ射影した量。
 * 正で大きいほど negative → positive (design_gap 方向) へ動かす良い改善意見。
 */
export function scoreOpinion(opinionVec: number[], currentVec: number[], designGap: number[]): number {
  const movement = subtract(opinionVec, currentVec);
  return scalarProjection(movement, designGap);
}

/**
 * 候補意見をスコアリングする。
 * @param opinionToVector 意見テキスト → 20 次元ベクトル (既定は textToVector)
 */
export function scoreOpinions(args: {
  candidates: OpinionCandidate[];
  currentVec: number[];
  designGap: number[];
  opinionToVector: (text: string) => number[];
}): ScoredOpinion[] {
  const { candidates, currentVec, designGap, opinionToVector } = args;
  return candidates.map((c) => ({
    id: c.id,
    score: scoreOpinion(opinionToVector(c.text), currentVec, designGap),
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
