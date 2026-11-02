#pragma once
// src/flow/llm-fallback.ts の移植: FallbackLlm — primary を試し、失敗したら fallback に回す LLMClient。
// 議論フローは personaId を渡さない呼び出し (facilitator / 投票 / 結論 / 要約 等) が多い。worker-pool backend を
// そのまま繋ぐと personaId 未指定で即 ok:false になるため、ルーティング不能な呼び出しは claude -p に回す。
#include "persona_engine/llm/client.hpp"

namespace di::flow {

class FallbackLlm : public persona_engine::LLMClient {
public:
    FallbackLlm(persona_engine::LLMClientPtr primary, persona_engine::LLMClientPtr fallback)
        : primary_(std::move(primary)), fallback_(std::move(fallback)) {}
    persona_engine::LLMResult invoke(const persona_engine::LLMInvokeArgs& args) override;

private:
    persona_engine::LLMClientPtr primary_;
    persona_engine::LLMClientPtr fallback_;
};

} // namespace di::flow
