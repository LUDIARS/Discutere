#pragma once
// src/flow/cost-logger.ts の移植: LLM 呼び出しコスト + transcript ロガー (OVERVIEW §8, T1 spec §4)。
// LLMClient を包み、呼び出しごとに llm_call_log へ 1 行書き込む。元の LLMClient の挙動は変えない。
// トークン数を返さない backend は null 許容 (best-effort)。cost_usd が無ければ token × 単価で推定して補う。
#include <optional>
#include <string>

#include "persona_engine/llm/client.hpp"

namespace di::flow {

struct CostLogContext {
    std::string flow;
    std::string sessionId;
    std::optional<double> round;
    std::optional<double> turn;
    std::optional<std::string> role;
    std::optional<std::string> persona;
    std::string location;
    /** 記録する backend 名。未指定時は get_config().llm.backend。 */
    std::optional<std::string> backend;
};

/** with_cost_log が返す包み。inner は呼び出し側が生かしておく (参照で持つ)。 */
class CostLoggedClient : public persona_engine::LLMClient {
public:
    CostLoggedClient(persona_engine::LLMClient& inner, CostLogContext ctx) : inner_(inner), ctx_(std::move(ctx)) {}
    persona_engine::LLMResult invoke(const persona_engine::LLMInvokeArgs& args) override;

private:
    persona_engine::LLMClient& inner_;
    CostLogContext ctx_;
};

/** withCostLog(client, ctx)。 */
inline CostLoggedClient with_cost_log(persona_engine::LLMClient& client, CostLogContext ctx) {
    return CostLoggedClient(client, std::move(ctx));
}

} // namespace di::flow
