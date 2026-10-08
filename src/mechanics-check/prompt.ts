/** Compare rules without attributing scenario/placement defects to mechanics. */
import type { MechanicsCheckRequest } from "./contracts.js";
import { MECHANICS_DIFFERENCE_KINDS } from "./contracts.js";
import { suppliedDocuments } from "./request.js";
import { DIAGNOSTIC_PROMPT_CONTRACT } from "../design-diagnostic/prompt-contract.js";
export function buildMechanicsCheckPrompt(request: MechanicsCheckRequest): { system: string; prompt: string } {
  return {
    system: `あなたは単発のメカニクス整合性診断器です。\n${DIAGNOSTIC_PROMPT_CONTRACT}
対象はルール、資源の生成/消費/条件/循環、M→D→Aの因果、ルール依存、改訂差だけです。
配置・遭遇・出現条件・技能・予測可能性の問題はレベル領域でも生じます。それだけでルールの欠陥と断定しないでください。
領域をまたぐ原因は条件と資料が十分ならboth、足りなければunknown。場面診断は/level-checkの領域と条件不足を示すだけで、自動実行しない。
幅・上振れ/下振れ・確率・期待値・分散を生成/計算しないでください。
logic_differences.kindは${MECHANICS_DIFFERENCE_KINDS.join("/")}だけ。play_envelopeやqualitative_playを出力しないでください。`,
    prompt: JSON.stringify({ documents: [...suppliedDocuments(request).values()] }),
  };
}
