/**
 * 論破の判定 [4] (dialectic.md §4.5)。
 *
 * 論拠をぶつけ合った結果、論点の結論は 止揚 / 論破 / 結論なし のいずれか (outcome.ts)。
 * このファイルは論破 (一方の根拠がすべて崩れ、意見が出なくなった) をコードのみで判定する。
 * 止揚は synthesis.ts、どちらにも至らなければ結論なし。合意 (結論の同一性) はゴールに置かない (2026-10-13)。
 *
 * 根拠の扱い:
 *   - unchallenged (有効な反論を受けていない) / defended (反論に応答して守った) → 残る
 *   - challenged (有効な反論に応答できなかった) / conceded (譲歩した) → 崩れた
 * 前提ルールに違反した反論は driver が question に格下げ済みなので、根拠を崩さない。
 */

import type { PositionRecord } from "./store.js";

/** 残った根拠 id (1 つも無ければその主張は論破された)。 */
export function standingGroundIds(p: PositionRecord): string[] {
  return p.grounds.filter((g) => g.state === "unchallenged" || g.state === "defended").map((g) => g.id);
}

export interface RefutationResult {
  /** 根拠がすべて崩れた Position。両方なら 2 件。 */
  refuted: PositionRecord[];
  /** 論破の判定文 (一方だけ崩れた)。論破でなければ null。 */
  note: string | null;
  /** 双方の根拠がすべて崩れた (勝敗を付けず結論なし) ときの理由文。それ以外は null。 */
  undecided: string | null;
}

function scope(p: PositionRecord): string {
  return p.qualifier ? ` (限定: ${p.qualifier})` : "";
}

/** 論破の判定 (決定的)。 */
export function detectRefutation(a: PositionRecord, b: PositionRecord): RefutationResult {
  const refuted = [a, b].filter((p) => standingGroundIds(p).length === 0);
  if (refuted.length === 0) return { refuted, note: null, undecided: null };
  if (refuted.length === 2) {
    // どちらも支えられない = 今ある材料では決着できない。勝敗は付けない (neco レビュー 2026-10-13)。
    return {
      refuted,
      note: null,
      undecided:
        `双方の論拠がすべて崩れたため勝敗は付けない。「${a.claim}」「${b.claim}」のどちらも、` +
        `今ある材料では支えられない (決着には追加の材料が要る)`,
    };
  }
  const loser = refuted[0];
  const winner = loser === a ? b : a;
  return {
    refuted,
    undecided: null,
    note:
      `「${loser.claim}」${scope(loser)}は根拠がすべて崩れて論破された。` +
      `「${winner.claim}」${scope(winner)}が根拠 ${standingGroundIds(winner).join(", ")} で残る`,
  };
}
