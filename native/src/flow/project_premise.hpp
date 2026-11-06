#pragma once
// 議論タイプごとのプロジェクト前提。登録済みリポジトリの有無とは別の概念 (src/flow/project-premise.ts)。
#include <optional>
#include <string>
#include <string_view>

namespace di::flow {

struct ProjectPremise {
    const char* label;
    const char* description;
    const char* instruction;
};

struct ProjectPremises {
    ProjectPremise discussion;
    ProjectPremise improvement;
};

/** PROJECT_PREMISES */
const ProjectPremises& project_premises();

/** "# 議論の前提\n<label>: <instruction>" (discussion / improvement 以外は "")。 */
std::string project_premise(const std::optional<std::string>& flow);

/** 企画はテーマだけで開始できる。既存の他フローの入力条件は維持する。 */
bool requires_project_title(const std::optional<std::string>& flow);

} // namespace di::flow
