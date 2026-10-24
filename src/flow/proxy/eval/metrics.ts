/**
 * 代理の「本人らしさ」の指標と合否 (統合レビュー §2.3 / §1「合格基準」、S4)。純関数。
 *
 *   M3 判別 (必須): 本人の伏せた発言を、2 人の代理のどちらの人物のものか当てる (偶然 0.5)
 *   M1 推薦予測 (参考): 伏せたゲームを本人が勧めたかを当てる。多数派ベースライン (本人の好意的/否定的の多い方を
 *      常に答える) との差で見る。2026-10-24 の実測で束からは多数派を超えられなかった
 *      (推薦可否は本人の好みよりゲーム自体の出来で決まる部分が大きい) ため、合否には使わない
 *   M5 根拠付き率: 代理の論拠のうち本人の発言に根拠を持つ割合
 *   M6 誤帰属率: 本人の発言と申告した論拠のうち、判定で「根拠が主張を支えていない」とされた割合
 * 測れなかった指標は null。必須指標 (M3) が測れない・件数が足りないときは合格にしない。
 */

export interface ProxyEvalThresholds {
  /** M3 の下限。 */
  minDiscrimination: number;
  /** M3 に必要な伏せた発言の最少数 (足りなければ判定しない)。 */
  minDiscriminationTexts: number;
  /** M5 の下限 (測ったときだけ必須)。 */
  minGroundedRate: number;
  /** M6 の上限 (測ったときだけ必須)。 */
  maxMisattribution: number;
}

export const DEFAULT_EVAL_THRESHOLDS: ProxyEvalThresholds = {
  minDiscrimination: 0.75,
  minDiscriminationTexts: 8,
  minGroundedRate: 0.8,
  maxMisattribution: 0.05,
};

export type RecommendAnswer = "yes" | "no" | "unknown";

export interface RecommendationScore {
  total: number;
  correct: number;
  /** 「分からない」と答えた数 (正解にしない)。 */
  abstained: number;
  accuracy: number | null;
  majorityAccuracy: number | null;
  lift: number | null;
}

/** M1。majority は形成に使った好意的/否定的の件数から決める (同数は「勧める」)。 */
export function scoreRecommendations(
  labels: readonly boolean[],
  answers: readonly RecommendAnswer[],
  formation: { positive: number; negative: number }
): RecommendationScore {
  const total = labels.length;
  if (total === 0) return { total, correct: 0, abstained: 0, accuracy: null, majorityAccuracy: null, lift: null };
  let correct = 0;
  let abstained = 0;
  labels.forEach((label, i) => {
    const a = answers[i] ?? "unknown";
    if (a === "unknown") abstained++;
    else if ((a === "yes") === label) correct++;
  });
  const majority = formation.positive >= formation.negative;
  const majorityCorrect = labels.filter((l) => l === majority).length;
  const accuracy = correct / total;
  const majorityAccuracy = majorityCorrect / total;
  return { total, correct, abstained, accuracy, majorityAccuracy, lift: accuracy - majorityAccuracy };
}

export function rate(numerator: number, denominator: number): number | null {
  return denominator > 0 ? numerator / denominator : null;
}

export interface DiscriminationScore {
  total: number;
  correct: number;
  accuracy: number | null;
}

export interface ProxyEvalMetrics {
  discrimination: DiscriminationScore;
  /** 参考値。測らなかったときは null。 */
  recommendation: RecommendationScore | null;
  groundedRate: number | null;
  misattributionRate: number | null;
}

export type ProxyEvalVerdict = { status: "passed" | "failed" | "unevaluated"; reasons: string[] };

export function judgeProxyEval(m: ProxyEvalMetrics, t: ProxyEvalThresholds = DEFAULT_EVAL_THRESHOLDS): ProxyEvalVerdict {
  const d = m.discrimination;
  if (d.total < t.minDiscriminationTexts || d.accuracy === null) {
    return { status: "unevaluated", reasons: [`伏せた発言が ${t.minDiscriminationTexts} 件未満で判別を測れない (${d.total} 件)`] };
  }
  const reasons: string[] = [];
  if (d.accuracy < t.minDiscrimination) reasons.push(`判別 ${d.accuracy.toFixed(2)} < ${t.minDiscrimination}`);
  if (m.groundedRate !== null && m.groundedRate < t.minGroundedRate) {
    reasons.push(`根拠付き率 ${m.groundedRate.toFixed(2)} < ${t.minGroundedRate}`);
  }
  if (m.misattributionRate !== null && m.misattributionRate > t.maxMisattribution) {
    reasons.push(`誤帰属率 ${m.misattributionRate.toFixed(2)} > ${t.maxMisattribution}`);
  }
  return { status: reasons.length === 0 ? "passed" : "failed", reasons };
}
