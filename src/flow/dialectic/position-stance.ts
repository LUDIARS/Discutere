/**
 * 定立の向きの検査 (neco レビュー 2026-10-13)。
 *
 * 論点「メカニクスの十分な面白さ」で、反対側が「メカニクスだけで十分」と賛成側より強い肯定を張り、
 * 対立の軸がすり替わった。定立ができたら、主張が論点 (問い) に「はい」「いいえ」のどちらで答えているかを
 * 判定 LLM で確かめ、陣営と逆向きなら driver が 1 回だけ作り直す。障害・不明は作り直さない (degrade)。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { IssueRecord } from "./store.js";

export type StanceAnswer = "yes" | "no" | "unclear";

/** 判定プロンプト (テスト用に export)。 */
export function buildStancePrompt(issue: IssueRecord, claim: string, qualifier: string | null): string {
  return (
    `# 論点 (問い)\n${issue.title}\n` +
    `(問いの形でなければ「${issue.title} は成り立つか / 十分か」と読み替える)\n\n` +
    `# 主張\n${claim}\n限定: ${qualifier ?? "(明示なし)"}\n\n` +
    `この主張は論点に対して「はい」と答えているか「いいえ」と答えているかを判定し、ラベルを 1 つだけ返してください (説明不要):\n` +
    `- yes: 論点を肯定している (成り立つ / 十分だ / 正解だ)\n` +
    `- no: 論点を否定している (成り立たない / 不十分だ / 正解ではない)\n` +
    `- unclear: どちらとも言えない`
  );
}

export function parseStanceAnswer(text: string): StanceAnswer {
  const lower = text.toLowerCase();
  const found = (["yes", "no", "unclear"] as const)
    .map((label) => ({ label, index: lower.indexOf(label) }))
    .filter((x) => x.index >= 0)
    .sort((a, b) => a.index - b.index)[0];
  return found?.label ?? "unclear";
}

/** 陣営の向き (賛成 = yes、反対 = no)。 */
export function expectedAnswer(stance: "pro" | "con"): Exclude<StanceAnswer, "unclear"> {
  return stance === "pro" ? "yes" : "no";
}

/** 主張が陣営と逆向きか (unclear は逆向きと見なさない)。 */
export function isReversed(stance: "pro" | "con", answer: StanceAnswer): boolean {
  return answer !== "unclear" && answer !== expectedAnswer(stance);
}

export async function judgePositionStance(args: {
  issue: IssueRecord;
  claim: string;
  qualifier: string | null;
  /** withCostLog 済み判定 LLM (judgeModel, location="position-stance")。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}): Promise<StanceAnswer> {
  const { llm, model, warn = () => {} } = args;
  const result = await llm.invoke({
    prompt: buildStancePrompt(args.issue, args.claim, args.qualifier),
    ...(model ? { model } : {}),
    maxTokens: 50,
  });
  if (!result.ok) {
    warn(`定立の向きの判定 LLM エラー: ${result.error} — 作り直さずに続行 (degrade)`);
    return "unclear";
  }
  return parseStanceAnswer(result.text);
}

/** 作り直しのときに定立プロンプトへ足す一言。 */
export function stanceFeedback(stance: "pro" | "con", previousClaim: string): string {
  const side = stance === "pro" ? "賛成" : "反対";
  const want = stance === "pro" ? "論点に「はい」と答える (肯定する)" : "論点に「いいえ」と答える (否定する)";
  return (
    `前回の主張「${previousClaim}」は【${side}】の立場と逆向きでした。` +
    `${want}主張にしてください。相手陣営より強い肯定・否定にすり替えないこと。`
  );
}
