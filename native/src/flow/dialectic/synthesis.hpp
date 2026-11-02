#pragma once
// src/flow/dialectic/synthesis.ts の移植: 止揚 [4] — Synthesis 候補の生成 + 折衷ゲート + 敵対的批准 + 修正ループ。
// 批准は reject デフォルト、accept は「自分の根拠の過半数が preservedIds で確認できた」場合のみコードが有効にする。
// 修正ループはコードが最大 MAX_REVISIONS=2 回でカット。尽きたら agreed_disagree。
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "flow/dialectic/store.hpp"
#include "flow/effect_predict.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::dialectic {

inline constexpr int MAX_REVISIONS = 2;

std::string synthesis_template_for(const TensionType& type);

struct GeneratedSynthesis {
    std::string text;
    std::vector<std::string> preserves;
    std::vector<std::string> negates;
    std::optional<std::string> elevates;
    bool degraded = false;
};

struct BuildSynthesisPromptArgs {
    std::string issueTitle;
    TensionRecord tension;
    PositionRecord positionA;
    PositionRecord positionB;
    std::optional<std::vector<std::string>> missingFeedback;
};
std::string build_synthesis_prompt(const BuildSynthesisPromptArgs& args);

struct GenerateSynthesisArgs : BuildSynthesisPromptArgs {
    std::string paperSystem;
    std::optional<std::string> model;
    WarnFn warn;
};
GeneratedSynthesis generate_synthesis(persona_engine::LLMClient& llm, const GenerateSynthesisArgs& args);

/** elevation ゲート ("aufheben" | "compromise")。空・「両方やる」は compromise。LLM 失敗も compromise。 */
std::string elevation_gate(persona_engine::LLMClient& llm, const std::optional<std::string>& elevates,
                           const std::optional<std::string>& model = std::nullopt, const WarnFn& warn = {});

struct BuildRatifyPromptArgs {
    PositionRecord position;
    std::string personaName;
    std::string synthesisText;
    std::vector<std::string> preserves;
    std::optional<std::string> elevates;
};
std::string build_ratify_prompt(const BuildRatifyPromptArgs& args);

struct RatifyArgs : BuildRatifyPromptArgs {
    std::optional<std::string> model;
    WarnFn warn;
};
RatificationEntry ratify_synthesis(persona_engine::LLMClient& llm, const RatifyArgs& args);

struct SynthesisLoopArgs {
    std::string issueTitle;
    TensionRecord tension;
    PositionRecord positionA;
    PositionRecord positionB;
    std::map<std::string, std::string> personaNames;
    std::string paperSystem;
    persona_engine::LLMClient* genLlm = nullptr;
    persona_engine::LLMClient* gateLlm = nullptr;
    persona_engine::LLMClient* ratifyLlm = nullptr;
    std::optional<std::string> mainModel;
    std::optional<std::string> judgeModel;
    WarnFn warn;
};

struct SynthesisOutcome {
    SynthesisRecord record;
    /** "synthesized" | "compromised" | "agreed_disagree" */
    std::string tensionStatus;
};

/** 生成 → 折衷ゲート → 敵対的批准 → 修正ループ。LLM (gen/gate/ratify) は必須 (null は std::invalid_argument)。 */
SynthesisOutcome run_synthesis_loop(const SynthesisLoopArgs& args);

} // namespace di::flow::dialectic
