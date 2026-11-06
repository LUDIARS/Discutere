#include "mechanics_check/prompt.hpp"

#include "base/strings.hpp"
#include "design_diagnostic/prompt_contract.hpp"
#include "mechanics_check/request.hpp"

namespace di::mechanics_check {

design_diagnostic::DiagnosticPrompt build_mechanics_check_prompt(const MechanicsCheckRequest& request) {
    design_diagnostic::DiagnosticPrompt p;
    p.system = "あなたは単発のメカニクス整合性診断器です。\n" + design_diagnostic::diagnostic_prompt_contract() +
               "\n対象はルール、資源の生成/消費/条件/循環、M→D→Aの因果、ルール依存、改訂差だけです。"
               "\n配置・遭遇・出現条件・技能・予測可能性の問題はレベル領域でも生じます。それだけでルールの欠陥と断定しないでください。"
               "\n領域をまたぐ原因は条件と資料が十分ならboth、足りなければunknown。場面診断は/level-checkの領域と条件不足を示すだけで、自動実行しない。"
               "\n幅・上振れ/下振れ・確率・期待値・分散を生成/計算しないでください。"
               "\nlogic_differences.kindは" +
               str::join(mechanics_difference_kinds(), "/") + "だけ。play_envelopeやqualitative_playを出力しないでください。";
    Json documents = Json::array();
    const auto docs = supplied_documents(request);
    for (const auto& d : docs.values()) documents.push_back(design_diagnostic::to_json(d));
    p.prompt = json_stringify(Json{{"documents", documents}});
    return p;
}

} // namespace di::mechanics_check
