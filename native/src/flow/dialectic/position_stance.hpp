#pragma once
// src/flow/dialectic/position-stance.ts の移植: 定立の向きの検査。
// 主張が論点 (問い) に「はい」「いいえ」のどちらで答えているかを判定 LLM で確かめる。障害・不明は unclear。
#include <optional>
#include <string>

#include "flow/dialectic/store.hpp"
#include "flow/effect_predict.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::dialectic {

/** "yes" | "no" | "unclear" */
using StanceAnswer = std::string;

std::string build_stance_prompt(const IssueRecord& issue, const std::string& claim, const std::optional<std::string>& qualifier);
StanceAnswer parse_stance_answer(const std::string& text);
StanceAnswer expected_answer(const std::string& stance);
bool is_reversed(const std::string& stance, const StanceAnswer& answer);

struct JudgePositionStanceArgs {
    IssueRecord issue;
    std::string claim;
    std::optional<std::string> qualifier;
    std::optional<std::string> model;
    WarnFn warn;
};
StanceAnswer judge_position_stance(persona_engine::LLMClient& llm, const JudgePositionStanceArgs& args);
std::string stance_feedback(const std::string& stance, const std::string& previous_claim);

} // namespace di::flow::dialectic
