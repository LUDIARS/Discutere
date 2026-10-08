/** Compare fixed scene conditions without confusing placement defects with rule defects. */
import { DIAGNOSTIC_PROMPT_CONTRACT } from "../design-diagnostic/prompt-contract.js";
import { LEVEL_DIFFERENCE_KINDS, type LevelCheckRequest } from "./contracts.js";
import { suppliedLevelDocuments } from "./request.js";
export function buildLevelCheckPrompt(request: LevelCheckRequest): { system: string; prompt: string } {
  return {
    system: `あなたは単発のレベル整合性診断器です。\n${DIAGNOSTIC_PROMPT_CONTRACT}
対象は配置、遭遇、出現条件、技能帯、情報、開始状態、制限時間/試行回数、適用ルールを固定した場面です。
場面本文に示された目指す体験と条件を比較してください。模型のruleset/placementが未提示ならdesign_gapでunknownとしてください。
配置による技能・予測可能性・上振れ/下振れの問題はルールの欠陥とは限りません。根拠不足でmechanicsやbothと断定しないでください。
PSはplayer_skill。randomとの二択ではなくmixedも許容。技能帯間の差を同一技能帯のランダム分散と混同しないでください。
事象は入力の同時組合せだけ。独立性や欠けた確率を仮定しない。min/max、上振れ/下振れ、期待値、分散はサーバが別途計算します。
数値模型がない場合は定性的な場面診断のみ。精密な幅・確率を創作しないでください。
logic_differences.kindは${LEVEL_DIFFERENCE_KINDS.join("/")}だけ。play_envelopeは出力禁止です。`,
    prompt: JSON.stringify({ documents: [...suppliedLevelDocuments(request).values()], playModel: request.playModel ?? null }),
  };
}
