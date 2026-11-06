#pragma once
// src/flow/conclusion.ts の移植: 結論生成 (discussion.md step 9)。
// 全ラウンドの止揚 + 高評価発話 (世論) を元に LLM で結論テキストを生成し flow_conclusion に保存する。
// facilitator/prompts の build_converge_prompt / extract_json_object を流用する。
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "flow/director.hpp"
#include "flow/effect_predict.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/vote.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

/** visualize/conclusions の CONVERGE_PREFIX に倣う。 */
inline constexpr std::string_view CONVERGE_PREFIX = "【収束】";
/** 構造化出力 (JSON) に 2 回失敗して生テキストへ degrade した結論のプレフィックス (respec 04, A7)。 */
inline constexpr std::string_view UNVERIFIED_PREFIX = "【収束(未検証)】";

struct ConclusionResult {
    std::string summary;
    bool concluded = false;
};

struct GenerateConclusionArgs {
    std::string theme;
    std::string sessionId;
    std::string paperId;
    std::vector<FlowUtteranceRecord> allUtterances;
    std::vector<std::string> allAufhebung;
    /** 全ラウンドのサマリ (respec 04, A5)。「# 各ラウンドの要約」節として渡す。 */
    std::vector<RoundSummary> rounds;
    std::vector<VoteResult> voteResults;
    /** 未指定は log::warn。 */
    WarnFn warn;
    /** フロー種別 (コストログ用)。 */
    std::string flow = "discussion";
    /** 結論に併記する注記 (09: 議論適性: 低のまま強行した場合など)。 */
    std::optional<std::string> annotation;
};

/** 全ラウンドサマリを converge プロンプトの節テキストにする (空なら "")。 */
std::string build_rounds_section(const std::vector<RoundSummary>& rounds);

/** 結論を生成して flow_conclusion テーブルに保存する。止揚も世論も無ければ「(結論なし)」。 */
ConclusionResult generate_conclusion(persona_engine::LLMClient& llm, const GenerateConclusionArgs& args);

} // namespace di::flow
