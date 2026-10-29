#pragma once
// src/persona-engine/llm/model-spec.ts — "<model>@<effort>" の分解と検査。
#include <optional>
#include <string>

namespace di::persona_engine {

struct ModelSpec {
    std::string model;
    std::optional<std::string> effort;
};

enum class ModelProvider { claude, codex };

ModelSpec parse_model_spec(const std::optional<std::string>& spec);
/** 問題があれば理由 (日本語)、無ければ nullopt。 */
std::optional<std::string> validate_model_spec(const ModelSpec& spec, ModelProvider provider);
bool is_codex_model(const std::optional<std::string>& spec);
std::optional<std::string> model_of_spec(const std::optional<std::string>& spec);

} // namespace di::persona_engine
