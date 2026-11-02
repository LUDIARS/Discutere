#include "flow/model_router.hpp"

#include "persona_engine/llm/model_spec.hpp"

namespace di::flow {

persona_engine::LLMResult ModelRouterLlm::invoke(const persona_engine::LLMInvokeArgs& args) {
    if (persona_engine::is_codex_model(args.model)) {
        if (!codex_) return persona_engine::LLMResult::failure("model-router: codex 経路なし");
        return codex_->invoke(args);
    }
    if (!claude_) return persona_engine::LLMResult::failure("model-router: claude 経路なし");
    return claude_->invoke(args);
}

} // namespace di::flow
