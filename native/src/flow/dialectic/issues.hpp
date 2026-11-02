#pragma once
// src/flow/dialectic/issues.ts の移植: 論点分解 [0] — 賛否が本気で割れる論点を分解する。
// dialectic エンジンの入口 [0] と議論適性ゲートが同一関数を共用する。失敗は std::runtime_error。
#include <optional>
#include <string>
#include <vector>

#include "flow/effect_predict.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::dialectic {

inline constexpr int ISSUES_MIN = 3;
inline constexpr int ISSUES_MAX = 5;

struct DecomposeIssuesArgs {
    std::string theme;
    std::optional<std::string> paperMd;
    std::optional<std::string> model;
    int maxIssues = ISSUES_MAX;
    WarnFn warn;
};

std::pair<std::string, std::string> build_decompose_prompt(const DecomposeIssuesArgs& args);
/** 論点を 0〜maxIssues 個に分解する。LLM 失敗 / JSON 解析失敗は std::runtime_error。 */
std::vector<std::string> decompose_issues(persona_engine::LLMClient& llm, const DecomposeIssuesArgs& args);

} // namespace di::flow::dialectic
