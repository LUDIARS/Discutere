#include "flow/llm_fallback.hpp"

namespace di::flow {

persona_engine::LLMResult FallbackLlm::invoke(const persona_engine::LLMInvokeArgs& args) {
    auto result = primary_ ? primary_->invoke(args) : persona_engine::LLMResult::failure("fallback-llm: primary なし");
    if (result.ok) return result;
    if (!fallback_) return result;
    return fallback_->invoke(args);
}

} // namespace di::flow
