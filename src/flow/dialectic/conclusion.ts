/**
 * dialectic の結論 (respec 06 §2「結論」, dialectic.md §6)。
 *
 * converge 入力を「直近 20 発話」でなく論証状態から組み立てる:
 *   - issue ごとの決着表 (止揚 / 論破 / 結論なし の 3 区分 + 結論なしの理由 — §4.5)
 *   - 未解決の事実問題 (unresolved_fact の resolution_note) — **結論本文に必ず明記** (誠実性)
 *   - レンズ投票の集中度
 *
 * 永続 (flow_conclusion / タイトルキャッシュ / 結論キャッシュ) は既存 generateConclusion を
 * 流用する (学習ビュー・議論一覧が dialectic セッションもそのまま表示できる)。
 * 未解決の事実問題は annotation としても併記するため、LLM が無視しても結論文に必ず残る。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { RoundSummary } from "../discussion-paper.js";
import type { FlowUtteranceRecord } from "../director.js";
import type { VoteResult } from "../vote.js";
import { generateConclusion, type ConclusionResult } from "../conclusion.js";
import type { IssueRecord, PositionRecord, SynthesisRecord, TensionRecord } from "./store.js";
import { issueOutcome, noConclusionReason, OUTCOME_LABEL, renderOutcomeLine } from "./outcome.js";

/** issue 1 件の決着スナップショット (driver が組み立てる)。 */
export interface IssueSettlement {
  issue: IssueRecord;
  positions: PositionRecord[];
  tensions: TensionRecord[];
  /** tension ごとの最終 Synthesis (無ければ空)。 */
  syntheses: SynthesisRecord[];
  /** 論破で決着した場合の判定文 (Tension を立てない)。論破でなければ null。 */
  refutation: string | null;
}

/** tension の決着 1 行を人間可読にする (止揚 / 結論なし + 理由)。 */
function settleTensionLine(
  t: TensionRecord,
  syntheses: readonly SynthesisRecord[]
): string {
  const s = syntheses.find((x) => x.tensionId === t.id);
  if (t.status === "synthesized") {
    return renderOutcomeLine("aufheben", `${s?.text ?? "(本文なし)"}${s?.elevates ? ` — 新しい枠: ${s.elevates}` : ""}`);
  }
  return renderOutcomeLine("none", noConclusionReason(t, s) ?? "(記録なし)");
}

/**
 * issue 決着を RoundSummary に射影する (round = issue ordinal)。
 * 既存の converge プロンプト「# 各ラウンドの要約」節とペーパー経過表示の両方がこれを読む。
 */
export function settlementToRoundSummary(
  s: IssueSettlement,
  personaNames: ReadonlyMap<string, string>,
  winnerShare: number | null
): RoundSummary {
  const positionLines = s.positions
    .map((p) => `${personaNames.get(p.personaId) ?? p.personaId} (${p.stance}): ${p.claim}`)
    .join(" / ");
  const outcome = issueOutcome(s);
  const tensionLines = s.refutation
    ? renderOutcomeLine("refuted", s.refutation)
    : s.tensions.length > 0
      ? s.tensions.map((t) => settleTensionLine(t, s.syntheses)).join(" / ")
      : renderOutcomeLine("none", "対立が成立しなかった");
  const share = winnerShare === null ? "" : ` / 投票集中度 ${winnerShare.toFixed(2)}`;
  const aufhebung = s.tensions
    .filter((t) => t.status === "synthesized")
    .map((t) => s.syntheses.find((x) => x.tensionId === t.id)?.text)
    .filter((t): t is string => !!t);
  return {
    round: s.issue.ordinal,
    summary: `論点「${s.issue.title}」【${OUTCOME_LABEL[outcome]}】 — ${positionLines} ⇒ ${tensionLines}${share}`,
    // 止揚だけを aufhebung に数える (折衷は summary に「折衷」として残る。dialectic.md §4)。
    aufhebung,
  };
}

/** 全 settlement から未解決の事実問題を集める。 */
export function collectUnresolvedFacts(settlements: readonly IssueSettlement[]): string[] {
  const notes: string[] = [];
  for (const s of settlements) {
    for (const t of s.tensions) {
      if (t.status === "unresolved_fact" && t.resolutionNote) notes.push(t.resolutionNote);
    }
  }
  return notes;
}

export interface ConcludeDialecticArgs {
  theme: string;
  sessionId: string;
  paperId: string;
  flow: string;
  settlements: readonly IssueSettlement[];
  personaNames: ReadonlyMap<string, string>;
  allUtterances: FlowUtteranceRecord[];
  voteResults: VoteResult[];
  /** 批准済み aufheben の統合文 (折衷は含めない)。flow_conclusion.aufhebung_json に永続。 */
  ratifiedAufheben: string[];
  llm: LLMClient;
  /** 議論適性: 低のまま強行した場合の注記 (09 と同じ扱い)。 */
  debatabilityNote?: string;
  warn?: (msg: string) => void;
}

/**
 * dialectic の結論を生成・永続する。generateConclusion (既存) を流用し、
 * rounds = issue 決着表 / annotation = 未解決の事実問題 (必ず明記) を渡す。
 */
export async function concludeDialectic(args: ConcludeDialecticArgs): Promise<ConclusionResult> {
  const { settlements, personaNames, voteResults } = args;

  const rounds: RoundSummary[] = settlements.map((s, i) =>
    settlementToRoundSummary(s, personaNames, voteResults[i]?.winnerShare ?? null)
  );

  const unresolvedFacts = collectUnresolvedFacts(settlements);
  const annotationParts: string[] = [];
  if (unresolvedFacts.length > 0) {
    annotationParts.push(`未解決の事実問題: ${unresolvedFacts.join(" / ")}`);
  }
  if (args.debatabilityNote) {
    annotationParts.push(args.debatabilityNote);
  }

  return generateConclusion({
    theme: args.theme,
    sessionId: args.sessionId,
    paperId: args.paperId,
    allUtterances: args.allUtterances,
    allAufhebung: args.ratifiedAufheben,
    rounds,
    voteResults,
    llm: args.llm,
    warn: args.warn,
    flow: args.flow,
    annotation: annotationParts.length > 0 ? annotationParts.join(" / ") : undefined,
  });
}
