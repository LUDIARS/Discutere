#pragma once
// src/persona-engine/llm/llm-vg.ts — LLM 呼び出しを Vestigium 形式 (channel=llm) の JSONL に書く。
#include <optional>
#include <string>

namespace di::persona_engine {

struct LlmLogArgs {
    std::string backend;
    std::string model;
    std::optional<std::string> kind;
    std::optional<std::string> system;
    std::string prompt;
    std::optional<double> input_tokens;
    std::optional<double> output_tokens;
    std::optional<double> cache_read_tokens;
    std::optional<double> cache_creation_tokens;
    std::optional<double> cost_usd;
    bool ok = false;
    std::optional<std::string> error;
};

/** 失敗しても投げない。 */
void log_llm(const LlmLogArgs& args);

} // namespace di::persona_engine
