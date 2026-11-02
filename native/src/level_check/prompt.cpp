#include "level_check/prompt.hpp"

#include "base/strings.hpp"
#include "design_diagnostic/prompt_contract.hpp"
#include "level_check/request.hpp"

namespace di::level_check {

design_diagnostic::DiagnosticPrompt build_level_check_prompt(const LevelCheckRequest& request) {
    design_diagnostic::DiagnosticPrompt p;
    p.system = "あなたは単発のレベル整合性診断器です。\n" + design_diagnostic::diagnostic_prompt_contract() +
               "\n対象は配置、遭遇、出現条件、技能帯、情報、開始状態、制限時間/試行回数、適用ルールを固定した場面です。"
               "\n場面本文に示された目指す体験と条件を比較してください。模型のruleset/placementが未提示ならdesign_gapでunknownとしてください。"
               "\n配置による技能・予測可能性・上振れ/下振れの問題はルールの欠陥とは限りません。根拠不足でmechanicsやbothと断定しないでください。"
               "\nPSはplayer_skill。randomとの二択ではなくmixedも許容。技能帯間の差を同一技能帯のランダム分散と混同しないでください。"
               "\n事象は入力の同時組合せだけ。独立性や欠けた確率を仮定しない。min/max、上振れ/下振れ、期待値、分散はサーバが別途計算します。"
               "\n数値模型がない場合は定性的な場面診断のみ。精密な幅・確率を創作しないでください。"
               "\nlogic_differences.kindは" +
               str::join(level_difference_kinds(), "/") + "だけ。play_envelopeは出力禁止です。";
    Json documents = Json::array();
    const auto docs = supplied_level_documents(request);
    for (const auto& d : docs.values()) documents.push_back(design_diagnostic::to_json(d));
    p.prompt = json_stringify(Json{{"documents", documents}, {"playModel", request.playModel ? to_json(*request.playModel) : Json(nullptr)}});
    return p;
}

} // namespace di::level_check
