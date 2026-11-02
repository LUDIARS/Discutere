#include "flow/project_premise.hpp"

namespace di::flow {

const ProjectPremises& project_premises() {
    static const ProjectPremises p{
        {"企画/議論", "プロジェクトなし。目的やアイデアから企画を検討する",
         "既存プロジェクトがない企画段階として議論する。目的・アイデア・比較案から検討し、未定の仕様や現状を既存の事実として作らない。既存作品や外部の声は参考事例として区別する。プロジェクト名や既存実装の提出を開始条件にしない。"},
        {"改善/議論", "プロジェクトあり。現状と課題を踏まえて改善を検討する",
         "対象の既存プロジェクトがある前提で議論する。対象・現状・課題・制約を確認し、現状に対する変更案と効果を比較する。不明な現状は推測で埋めず、必要な情報を質問する。"},
    };
    return p;
}

std::string project_premise(const std::optional<std::string>& flow) {
    if (!flow || (*flow != "discussion" && *flow != "improvement")) return "";
    const auto& premise = *flow == "discussion" ? project_premises().discussion : project_premises().improvement;
    return std::string("# 議論の前提\n") + premise.label + ": " + premise.instruction;
}

bool requires_project_title(const std::optional<std::string>& flow) { return !flow || *flow != "discussion"; }

} // namespace di::flow
