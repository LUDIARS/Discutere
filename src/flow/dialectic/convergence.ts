/**
 * 計測型収束 (respec 06 §2, dialectic.md §6) — コードのみで判定 (LLM なし)。
 *
 * 早期収束は「止揚 N 件」ではなく状態の計測値の多数決:
 *   (a) 未応答反論数 = 0 (graph.unresolvedRebuttals().length === 0)
 *   (b) レンズ投票の集中度 winnerShare >= flow.convergeShare
 *   (c) 新規 claim の枯渇 (直近 K=flow.dialectic.staleTurns ターン新しい主張なし)
 * **2/3 成立で収束**。`facilitator.aufhebungTarget` は「批准済み aufheben 数の上限キャップ」
 * としてのみ参照する (到達で必ず収束。折衷 compromise は数えない — dialectic.md §4)。
 */

export interface ConvergenceInput {
  /** graph.unresolvedRebuttals().length。 */
  unresolvedRebuttals: number;
  /** 直近 issue 決着投票の最多得票シェア (無投票/算出不能は null)。 */
  winnerShare: number | null;
  /** 収束に要求する集中度 (config flow.convergeShare)。 */
  convergeShare: number;
  /** graph.newClaimsSince(now - K) の値 (直近 K ターンの新規 claim 数)。 */
  newClaimsInWindow: number;
  /** 批准済み aufheben 数 (kind=compromise は含めない)。 */
  ratifiedAufhebenCount: number;
  /** 上限キャップ (config facilitator.aufhebungTarget)。 */
  aufhebungTarget: number;
}

export interface ConvergenceVerdict {
  converged: boolean;
  /** シグナル別の成立状況。 */
  signals: {
    rebuttalsSettled: boolean;
    voteConcentrated: boolean;
    claimsStale: boolean;
  };
  /** 成立シグナル数 (0..3)。 */
  met: number;
  /** aufhebungTarget キャップ到達 (到達で必ず収束)。 */
  capReached: boolean;
  /** ログ用の一言。 */
  note: string;
}

/** 計測型収束の判定 (純関数・決定的)。 */
export function assessConvergence(input: ConvergenceInput): ConvergenceVerdict {
  const rebuttalsSettled = input.unresolvedRebuttals === 0;
  const voteConcentrated = input.winnerShare !== null && input.winnerShare >= input.convergeShare;
  const claimsStale = input.newClaimsInWindow === 0;
  const met = [rebuttalsSettled, voteConcentrated, claimsStale].filter(Boolean).length;
  const capReached =
    input.aufhebungTarget > 0 && input.ratifiedAufhebenCount >= input.aufhebungTarget;
  const converged = capReached || met >= 2;

  const parts = [
    `未応答反論=${input.unresolvedRebuttals}${rebuttalsSettled ? " ✓" : ""}`,
    `投票集中度=${input.winnerShare === null ? "n/a" : input.winnerShare.toFixed(2)}${voteConcentrated ? " ✓" : ""}`,
    `新規claim(窓内)=${input.newClaimsInWindow}${claimsStale ? " ✓" : ""}`,
  ];
  const capNote = capReached
    ? ` / 批准済み止揚 ${input.ratifiedAufhebenCount} 件が上限キャップ ${input.aufhebungTarget} に到達`
    : "";
  return {
    converged,
    signals: { rebuttalsSettled, voteConcentrated, claimsStale },
    met,
    capReached,
    note: `${parts.join(" / ")} → ${met}/3${capNote}${converged ? " → 収束" : ""}`,
  };
}
