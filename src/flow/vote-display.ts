/**
 * 投票結果の表示整形 (respec 03, A10 — tally の意味分離)。
 *
 * discord-live / slack-live の得票可視化が共有する純関数。
 * kind="votes" は「n 票」、kind="scores" (improvement の design_gap 射影) は
 * 「スコア x.xx (design_gap 適合)」表記にし、機械スコアを票数として見せない。
 * トランスポート非依存 (絵文字/リアクション付与は各アダプタ側)。
 */

import type { VoteEvent } from "./director.js";

/** この評価種別で 👍 (得票リアクション) を枚数積みしてよいか。scores は 🏆 のみ。 */
export function shouldStackVoteReactions(kind: VoteEvent["kind"]): boolean {
  return kind !== "scores";
}

/** 可視化すべき結果があるか (votes: 有効票あり / scores: winner あり)。 */
export function hasVisibleVoteResult(e: Pick<VoteEvent, "kind" | "tally" | "winner">): boolean {
  if (e.kind === "scores") return e.winner !== null;
  return Object.values(e.tally).reduce((s, n) => s + n, 0) > 0;
}

/**
 * 得票/スコア集計のランキング行 (降順) を組み立てる。
 * votes は正の得票のみ、scores は全候補 (負値もあり得る) を降順で出す。
 */
export function buildVoteRankingLines(
  e: Pick<VoteEvent, "kind" | "tally" | "winner">,
  nameOf: (utteranceId: string) => string
): string[] {
  const isScores = e.kind === "scores";
  return Object.entries(e.tally)
    .filter(([, n]) => (isScores ? true : n > 0))
    .sort((a, b) => b[1] - a[1])
    .map(([id, n], i) => {
      const name = nameOf(id);
      const medal = id === e.winner ? "🏆" : `${i + 1}.`;
      const value = isScores ? `スコア ${n.toFixed(2)} (design_gap 適合)` : `${n}票`;
      return `${medal} ${name} — ${value}`;
    });
}

/** 集計投稿のタイトル行。scores は「投票」でなく「機械スコア」と明示する。 */
export function voteNoticeTitle(round: number, kind: VoteEvent["kind"]): string {
  return kind === "scores"
    ? `📐 ラウンド ${round} 機械スコア (design_gap 適合)`
    : `🗳️ ラウンド ${round} 投票結果`;
}
