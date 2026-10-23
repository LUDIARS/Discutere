/**
 * 本人らしさの評価で LLM に聞く 3 種類の問い (S4)。prompt と応答の読み取りだけを持つ。
 *   - 推薦予測 (M1): 束だけを材料に、伏せたゲームを勧めるか
 *   - 判別 (M3): 伏せた発言が、2 人の束のどちらの人物のものか
 *   - 根拠の支持 (M6): 本人の発言 [E番号] が、代理の主張を支えているか
 * LLM 障害は「分からない」として扱う (正解に数えない = 甘くしない)。
 */

import type { LLMClient } from "../../../persona-engine/llm/client.js";
import type { RecommendAnswer } from "./metrics.js";

export function buildRecommendPrompt(bundleText: string, gameTitle: string): string {
  return (
    `${bundleText}\n\n` +
    `# 問い\nこの人物はゲーム「${gameTitle}」を人に勧めると思いますか。\n` +
    `上の主張と発言だけを材料に、次のラベルを 1 つだけ返してください (説明不要):\n` +
    `- yes: 勧める\n- no: 勧めない\n- unknown: 材料からは分からない`
  );
}

export function parseRecommend(text: string): RecommendAnswer {
  const lower = text.toLowerCase();
  const hits = (["unknown", "yes", "no"] as const)
    .map((label) => ({ label, index: lower.search(new RegExp(`\\b${label}\\b`)) }))
    .filter((x) => x.index >= 0)
    .sort((a, b) => a.index - b.index);
  return hits[0]?.label ?? "unknown";
}

export async function askRecommend(llm: LLMClient, bundleText: string, gameTitle: string, model?: string): Promise<RecommendAnswer> {
  const r = await llm.invoke({ prompt: buildRecommendPrompt(bundleText, gameTitle), maxTokens: 10, ...(model ? { model } : {}) });
  return r.ok ? parseRecommend(r.text) : "unknown";
}

export function buildDiscriminatePrompt(bundles: readonly [string, string], utterance: string): string {
  return (
    `# 人物 1\n${bundles[0]}\n\n# 人物 2\n${bundles[1]}\n\n` +
    `# 発言\n${utterance}\n\n` +
    `この発言を書いたのは人物 1 と人物 2 のどちらだと思いますか。数字 1 か 2 だけを返してください (説明不要)。`
  );
}

export function parseDiscriminate(text: string): 1 | 2 | null {
  const m = text.match(/[12]/);
  return m ? (Number(m[0]) as 1 | 2) : null;
}

export async function askDiscriminate(
  llm: LLMClient,
  bundles: readonly [string, string],
  utterance: string,
  model?: string
): Promise<1 | 2 | null> {
  const r = await llm.invoke({ prompt: buildDiscriminatePrompt(bundles, utterance), maxTokens: 5, ...(model ? { model } : {}) });
  return r.ok ? parseDiscriminate(r.text) : null;
}

export function buildSupportPrompt(evidence: readonly string[], statement: string): string {
  return (
    `# 本人の発言\n${evidence.map((e) => `- ${e}`).join("\n")}\n\n` +
    `# 主張\n${statement}\n\n` +
    `上の本人の発言は、この主張を支えていますか。ラベルを 1 つだけ返してください (説明不要):\n` +
    `- supported: 発言から言える\n- unsupported: 発言からは言えない / 発言と違う`
  );
}

/** 支えていれば true、支えていない / 判定できなければ false (誤帰属側に数える = 甘くしない)。 */
export async function askSupported(llm: LLMClient, evidence: readonly string[], statement: string, model?: string): Promise<boolean> {
  const r = await llm.invoke({ prompt: buildSupportPrompt(evidence, statement), maxTokens: 10, ...(model ? { model } : {}) });
  if (!r.ok) return false;
  const lower = r.text.toLowerCase();
  return lower.includes("supported") && !lower.includes("unsupported");
}
