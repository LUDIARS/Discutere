#pragma once
// src/flow/dialectic/tension.ts の移植: 矛盾の特定 [3] — Tension の型分類 + 事実対立の RAG 解消。
// 分類は判定 LLM の単一ラベル (失敗は values に倒す)。事実対立は外部の声で照会し、決着しなければ unresolved_fact。
#include <optional>
#include <string>
#include <vector>

#include "flow/dialectic/store.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/effect_predict.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::dialectic {

std::string build_classify_prompt(const IssueRecord& issue, const PositionRecord& a, const PositionRecord& b);
std::optional<TensionType> parse_tension_type(const std::string& text);

struct ClassifyTensionArgs {
    IssueRecord issue;
    PositionRecord positionA;
    PositionRecord positionB;
    std::optional<std::string> model;
    WarnFn warn;
};
TensionType classify_tension(persona_engine::LLMClient& llm, const ClassifyTensionArgs& args);

std::string build_fact_resolve_prompt(const IssueRecord& issue, const PositionRecord& a, const PositionRecord& b,
                                      const std::vector<ContextVoice>& evidence);
/** "a" | "b" | "none" | nullopt */
std::optional<std::string> parse_fact_verdict(const std::string& text);

struct FactResolution {
    /** "resolved" | "unresolved_fact" */
    std::string status;
    std::string note;
};

struct ResolveFactArgs {
    std::string theme;
    IssueRecord issue;
    PositionRecord positionA;
    PositionRecord positionB;
    /** 外部の声 RAG (KG が無い環境では空を返す)。 */
    ListExternalVoicesFn listExternalVoices;
    std::optional<std::string> model;
    WarnFn warn;
};
FactResolution resolve_fact_tension(persona_engine::LLMClient& llm, const ResolveFactArgs& args);

} // namespace di::flow::dialectic
