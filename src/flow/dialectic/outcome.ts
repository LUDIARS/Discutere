/**
 * 論点の結論の 3 区分 (dialectic.md §4.5, 2026-10-13) — 決定的コード・LLM 不使用。
 *
 * 論拠をぶつけ合った結果、論点の結論は次のいずれかになる:
 *   - 止揚 (aufheben) : 止揚候補が両陣営に批准された (tension status = synthesized)
 *   - 論破 (refuted)  : 一方の根拠がすべて崩れた (goal.ts の detectRefutation)
 *   - 結論なし (none)  : それ以外 (折衷・批准不成立・事実の照会結果・打ち切り)。理由を添える
 * 合意 (結論の同一性) はゴールに置かない。旧 agreed 行は読み出し互換のため「結論なし」の理由として表示する。
 */

import type { SynthesisRecord, TensionRecord } from "./store.js";

export type IssueOutcome = "aufheben" | "refuted" | "none";

export const OUTCOME_LABEL: Record<IssueOutcome, string> = {
  aufheben: "止揚",
  refuted: "論破",
  none: "結論なし",
};

/** 論点の結論区分。論破は Tension を立てないので refutation の有無で先に決める。 */
export function issueOutcome(args: { refutation: string | null; tensions: readonly TensionRecord[] }): IssueOutcome {
  if (args.refutation) return "refuted";
  if (args.tensions.some((t) => t.status === "synthesized")) return "aufheben";
  return "none";
}

/** 「結論なし」の理由 (Tension の状態から)。止揚・論破の Tension には null。 */
export function noConclusionReason(t: TensionRecord, synthesis?: SynthesisRecord): string | null {
  switch (t.status) {
    case "synthesized":
      return null;
    case "compromised":
      return `折衷案は批准されたが止揚ではない${synthesis ? ` — 折衷案: ${synthesis.text}` : ""}`;
    case "agreed_disagree":
      return `止揚案が両陣営に批准されず対立が残った${synthesis ? ` — 最終候補: ${synthesis.text}` : ""}`;
    case "resolved":
      return `事実の対立は照会で確かめた: ${t.resolutionNote ?? "(記録なし)"}`;
    case "unresolved_fact":
      return `未解決の事実問題: ${t.resolutionNote ?? "(記録なし)"}`;
    case "agreed":
      return `(旧方式の合意判定) ${t.resolutionNote ?? "(記録なし)"}`;
    case "aligned":
    case "partially_aligned":
      return "(旧方式のすり合わせ・詳細は記録なし)";
    case "open":
      return "議論を打ち切った";
  }
}

/** 進行役が流す結論の一言 (「止揚: …」「論破: …」「結論なし: …」)。 */
export function renderOutcomeLine(outcome: IssueOutcome, detail: string): string {
  return `${OUTCOME_LABEL[outcome]}: ${detail}`;
}
