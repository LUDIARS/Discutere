/**
 * 代理の「本人らしさ」の指標と合否 (統合レビュー §2.3 / §1「合格基準」、S4)。純関数。
 *
 *   M1 推薦予測: 伏せたゲームを本人が勧めたかを当てる。多数派ベースライン (本人の好意的/否定的の多い方を
 *      常に答える) との差で見る (全部好意的な人は多数派だけで当たるため)
 *   M3 判別: 本人の伏せた発言を、2 人の代理のどちらの人物のものか当てる (偶然 0.5)
 *   M5 根拠付き率: 代理の論拠のうち本人の発言に根拠を持つ割合
 *   M6 誤帰属率: 本人の発言と申告した論拠のうち、判定で「根拠が主張を支えていない」とされた割合
 * 測れなかった指標は null。必須指標 (M1) が測れない・件数が足りないときは合格にしない。
 */

export interface ProxyEvalThresholds {
  /** M1 の正解率の下限。 */
  minAccuracy: number;
  /** M1 で多数派ベースラインを上回る幅。 */
  minLiftOverMajority: number;
  /** M1 に必要な伏せたゲームの最少数 (足りなければ判定しない)。 */
  minHeldOut: number;
  /** M3 の下限 (測ったときだけ必須)。 */
  minDiscrimination: number;
  /** M5 の下限 (測ったときだけ必須)。 */
  minGroundedRate: number;
  /** M6 の上限 (測ったときだけ必須)。 */
  maxMisattribution: number;
}

export const DEFAULT_EVAL_THRESHOLDS: ProxyEvalThresholds = {
  minAccuracy: 0.7,
  minLiftOverMajority: 0.15,
  minHeldOut: 3,
  minDiscrimination: 0.75,
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

export interface ProxyEvalMetrics {
  recommendation: RecommendationScore;
  discrimination: number | null;
  groundedRate: number | null;
  misattributionRate: number | null;
}

export type ProxyEvalVerdict = { status: "passed" | "failed" | "unevaluated"; reasons: string[] };

export function judgeProxyEval(m: ProxyEvalMetrics, t: ProxyEvalThresholds = DEFAULT_EVAL_THRESHOLDS): ProxyEvalVerdict {
  const r = m.recommendation;
  if (r.total < t.minHeldOut || r.accuracy === null || r.lift === null) {
    return { status: "unevaluated", reasons: [`伏せたゲームが ${t.minHeldOut} 本未満で推薦予測を測れない (${r.total} 本)`] };
  }
  const reasons: string[] = [];
  if (r.accuracy < t.minAccuracy) reasons.push(`推薦予測 ${r.accuracy.toFixed(2)} < ${t.minAccuracy}`);
  if (r.lift < t.minLiftOverMajority) reasons.push(`多数派との差 ${r.lift.toFixed(2)} < ${t.minLiftOverMajority}`);
  if (m.discrimination !== null && m.discrimination < t.minDiscrimination) {
    reasons.push(`判別 ${m.discrimination.toFixed(2)} < ${t.minDiscrimination}`);
  }
  if (m.groundedRate !== null && m.groundedRate < t.minGroundedRate) {
    reasons.push(`根拠付き率 ${m.groundedRate.toFixed(2)} < ${t.minGroundedRate}`);
  }
  if (m.misattributionRate !== null && m.misattributionRate > t.maxMisattribution) {
    reasons.push(`誤帰属率 ${m.misattributionRate.toFixed(2)} > ${t.maxMisattribution}`);
  }
  return { status: reasons.length === 0 ? "passed" : "failed", reasons };
}
