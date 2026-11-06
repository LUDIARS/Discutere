#pragma once
// src/flow/model-router.ts の移植: ModelRouterLlm — model spec の provider で LLMClient を振り分ける。
// `gpt-` 接頭辞を Codex 経路 (persona_engine/llm/codex_cli) へ、それ以外を既存チェーン (claude) へ回す。
// Codex 経路が無い / 失敗した場合は Claude へフォールバックしない (「GPT の意見」を偽装しないため)。
#include "persona_engine/llm/client.hpp"

namespace di::flow {

class ModelRouterLlm : public persona_engine::LLMClient {
public:
    /** @implements SPEC-FLOW-MODEL-ROSTER — provider ごとのクライアントを固定する。codex は nullptr 可。 */
    ModelRouterLlm(persona_engine::LLMClientPtr claude, persona_engine::LLMClientPtr codex)
        : claude_(std::move(claude)), codex_(std::move(codex)) {}
    /** @implements SPEC-FLOW-MODEL-ROSTER — gpt-* だけを Codex 経路へ送る。 */
    persona_engine::LLMResult invoke(const persona_engine::LLMInvokeArgs& args) override;

private:
    persona_engine::LLMClientPtr claude_;
    persona_engine::LLMClientPtr codex_;
};

} // namespace di::flow
