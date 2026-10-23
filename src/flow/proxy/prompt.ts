/**
 * 代理議論で定立・応酬の prompt に足す部分 (S3)。
 *
 * 代理のときだけ、束 (本人の主張と発言) と、論拠ごとに申告させる 2 項目 (claimKind / evidenceIds) の
 * 説明を足す。代理でなければ何も足さない (通常議論の prompt はバイト等価のまま)。
 */

import type { FlowPersona } from "../personas.js";

/** 束と申告の決まり。代理でなければ空配列。 */
export function proxyPromptLines(persona: FlowPersona): string[] {
  if (!persona.proxy) return [];
  return [
    persona.proxy.bundleText,
    "",
    `# 論拠ごとに申告すること (代理の決まり)`,
    `- claimKind: 本人の発言との関係を次の 1 つで書く`,
    `  - sourceQuote: 本人の発言をそのまま引く (引く部分を「」で囲み data に入れる)`,
    `  - groundedParaphrase: 本人の発言に基づいて言い換える`,
    `  - simulationInference: 本人の傾向からの推測 (本人はそう言っていない)`,
    `  - unknown: 材料が無くて分からない`,
    `- evidenceIds: 根拠にした本人の発言の id の配列 (例 ["E2","E5"])。sourceQuote / groundedParaphrase では必須。`,
    `  上に無い id を書かない。推測を本人の発言として書かない。`,
    "",
  ];
}

/** JSON の論拠 1 件に足す項目の書式 (代理のときだけ)。 */
export function proxyGroundFields(persona: FlowPersona): string {
  return persona.proxy
    ? `, "claimKind": "<sourceQuote | groundedParaphrase | simulationInference | unknown>", "evidenceIds": ["<E番号>"]`
    : "";
}
