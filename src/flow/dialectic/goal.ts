/**
 * 論点のゴール判定 [4] (dialectic.md §4.5)。
 *
 * AI 同士の議論は「作りたい体験・試したい施策・他ゲームの事例についての共通見解は取れている」
 * 前提で進め、すり合わせはゴールに置かない。論点のゴールは次のいずれかの状態:
 *   1. 論破 (refuted)  — 一方の根拠がすべて崩れ、意見が出なくなった。コードのみで判定。
 *   2. 合意 (agreed)   — ジンテーゼ。両者の主張が限定の範囲で同じ結論になった (結論の同一性の担保)。判定 LLM。
 *   3. 止揚 (synthesized) — 止揚候補が両陣営に批准された。既存の synthesis ループ (synthesis.ts)。
 * driver がこの順に試し、どれにも達しなければゴール未到達として記録する。
 *
 * 根拠の扱い:
 *   - unchallenged (有効な反論を受けていない) / defended (反論に応答して守った) → 残る
 *   - challenged (有効な反論に応答できなかった) / conceded (譲歩した) → 崩れた
 * 前提ルールに違反した反論は driver が question に格下げ済みなので、根拠を崩さない。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { IssueRecord, PositionRecord } from "./store.js";

/** 残った根拠 id (1 つも無ければその主張は論破された)。 */
export function standingGroundIds(p: PositionRecord): string[] {
  return p.grounds.filter((g) => g.state === "unchallenged" || g.state === "defended").map((g) => g.id);
}

export interface RefutationResult {
  /** 論破された Position (根拠がすべて崩れた側)。両方なら 2 件。 */
  refuted: PositionRecord[];
  /** 結論・露出用の判定文。論破が無ければ null。 */
  note: string | null;
}

function scope(p: PositionRecord): string {
  return p.qualifier ? ` (限定: ${p.qualifier})` : "";
}

/** 論破の判定 (決定的)。 */
export function detectRefutation(a: PositionRecord, b: PositionRecord): RefutationResult {
  const refuted = [a, b].filter((p) => standingGroundIds(p).length === 0);
  if (refuted.length === 0) return { refuted, note: null };
  if (refuted.length === 2) {
    return { refuted, note: `双方の根拠がすべて崩れた: 「${a.claim}」「${b.claim}」はどちらも成り立たない` };
  }
  const loser = refuted[0];
  const winner = loser === a ? b : a;
  return {
    refuted,
    note:
      `「${loser.claim}」${scope(loser)}は根拠がすべて崩れて論破された。` +
      `「${winner.claim}」${scope(winner)}が根拠 ${standingGroundIds(winner).join(", ")} で残る`,
  };
}

function renderSide(label: string, p: PositionRecord): string {
  const grounds = p.grounds
    .filter((g) => g.state === "unchallenged" || g.state === "defended")
    .map((g) => `  - [${g.id}] データ: ${g.text}${g.warrant ? ` / 論拠: ${g.warrant}` : ""}`)
    .join("\n");
  return `${label}: ${p.claim}\n  限定: ${p.qualifier ?? "(明示なし)"}\n${grounds}`;
}

/** 合意 (ジンテーゼ = 結論の同一性) 判定プロンプト (テスト用に export)。 */
export function buildAgreementPrompt(issue: IssueRecord, a: PositionRecord, b: PositionRecord): string {
  return (
    `# 論点\n${issue.title}\n\n` +
    `# 議論を経て残った 2 つの主張 (崩れた根拠は除外済み)\n` +
    `${renderSide("A", a)}\n\n${renderSide("B", b)}\n\n` +
    `A と B は、それぞれの限定の範囲で同じ結論を述べているか (合意しているか) を判定し、ラベルを 1 つだけ返してください (説明不要):\n` +
    `- same: 言い方は違っても、同じ条件で同じ結論を述べている\n` +
    `- different: 結論が異なる、または条件が違うので同じとは言えない\n` +
    `判断に迷う場合は different を返す。`
  );
}

/** 応答からラベルを取り出す (最初に現れた既知ラベル)。 */
export function parseAgreementVerdict(text: string): "same" | "different" | null {
  const lower = text.toLowerCase();
  const s = lower.indexOf("same");
  const d = lower.indexOf("different");
  if (s < 0 && d < 0) return null;
  if (s < 0) return "different";
  if (d < 0) return "same";
  return s < d ? "same" : "different";
}

/**
 * 合意 (ジンテーゼ) を判定 LLM で確かめる。
 * 障害・ラベル不明は合意なしに倒す (合意を水増ししない、warn 明示)。
 */
export async function judgeAgreement(args: {
  issue: IssueRecord;
  positionA: PositionRecord;
  positionB: PositionRecord;
  /** withCostLog 済み判定 LLM (judgeModel, location="agreement")。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}): Promise<boolean> {
  const { llm, model, warn = () => {} } = args;
  const result = await llm.invoke({
    prompt: buildAgreementPrompt(args.issue, args.positionA, args.positionB),
    ...(model ? { model } : {}),
    maxTokens: 50,
  });
  if (!result.ok) {
    warn(`合意判定 LLM エラー: ${result.error} — 合意なしとして続行 (degrade)`);
    return false;
  }
  const verdict = parseAgreementVerdict(result.text);
  if (!verdict) {
    warn(`合意判定ラベル不明: "${result.text.slice(0, 60)}" — 合意なしとして続行 (degrade)`);
    return false;
  }
  return verdict === "same";
}

/** 合意成立時の判定文。 */
export function agreementNote(a: PositionRecord, b: PositionRecord): string {
  return `合意した (ジンテーゼ): 「${a.claim}」${scope(a)} と「${b.claim}」${scope(b)} は同じ結論を述べている`;
}
