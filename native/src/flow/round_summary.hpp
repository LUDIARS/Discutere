#pragma once
// src/flow/round-summary.ts の移植: ラウンドサマリ + 止揚 (discussion.md step 7)。
// ラウンドの発話を LLM でサマライズし、止揚があれば抽出して discussion_paper_round に追記する。
#include <string>
#include <vector>

#include "flow/discussion_paper.hpp"
#include "flow/effect_predict.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

struct RoundSummaryInput {
    std::string theme;
    std::string sessionId;
    std::string paperId;
    int round = 0;
    std::vector<NamedUtterance> utterances;
    /** 過去ラウンドで蓄積済みの止揚。 */
    std::vector<std::string> stockedAufhebung;
    /** 未指定は log::warn。 */
    WarnFn warn;
    std::string flow = "discussion";
};

struct RoundSummaryOutput {
    std::string summary;
    std::vector<std::string> aufhebung;
    /** 今ラウンドで新しく追加された止揚。 */
    std::vector<std::string> newAufhebung;
};

std::string build_round_summary_prompt(const std::string& theme, const std::vector<NamedUtterance>& utterances);
RoundSummaryOutput summarize_round(persona_engine::LLMClient& llm, const RoundSummaryInput& input);

} // namespace di::flow
