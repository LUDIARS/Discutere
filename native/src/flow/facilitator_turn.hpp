#pragma once
// src/flow/facilitator-turn.ts の移植: ファシリテーター開幕ターン (discussion.md step 4, respec 10 + respec 02)。
// ラウンド開幕の議題提示を担う。round ≥ 2 では前ラウンドの結果 (サマリ / 止揚 / 世論) をプロンプトに注入し、
// 前ラウンドの到達点から議論を進めさせる (A8)。facilitator は開幕ターン (turn 0) 専任。
#include <optional>
#include <string>
#include <vector>

#include "flow/director.hpp"
#include "flow/effect_predict.hpp"
#include "flow/personas.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

/** 開幕プロンプトに注入する前ラウンドの結果 (round ≥ 2 のみ)。 */
struct PreviousRoundContext {
    int round = 0;
    std::string summary;
    std::vector<std::string> aufhebung;
    struct Winner {
        std::string personaName;
        std::string text;
    };
    /** 前ラウンドの世論 (最多得票の発話)。無投票 / 全員棄権なら nullopt。 */
    std::optional<Winner> winner;
};

/** ファシリテーター開幕プロンプトを組み立てる (テスト用に公開)。 */
std::string build_facilitator_prompt(const std::string& theme, int round,
                                     const std::optional<PreviousRoundContext>& previous_round = std::nullopt,
                                     const std::vector<std::string>& guiding_issues = {});

struct FacilitatorTurnArgs {
    std::string theme;
    int round = 0;
    FlowPersona facilitator;
    std::string sessionId;
    std::string paperId;
    std::string flow = "discussion";
    std::optional<PreviousRoundContext> previousRound;
    /** 承認済み論点 (09、ペーパーゲート由来)。毎ラウンド参考注入する。 */
    std::vector<std::string> guidingIssues;
    WarnFn warn;
};

/** 開幕ターンを 1 回実行し、発話レコードを返す (LLM エラーは isError=true、空応答は nullopt)。 */
std::optional<FlowUtteranceRecord> run_facilitator_turn(persona_engine::LLMClient& llm, const FacilitatorTurnArgs& args);

} // namespace di::flow
