#pragma once
// tier ルーティング (src/persona-engine/llm/tier-routing.ts, @ludiars/llm-gateway 連携)。
// タスク種別から使うモデル ID を解決するだけ (実 dispatch は LLMClient backend が行う)。
#include <optional>
#include <string>

#include "lib/llm_gateway/routing.hpp"
#include "lib/llm_gateway/tokens.hpp"

namespace di::persona_engine {

struct TierModels {
    std::optional<std::string> cheap;
    std::optional<std::string> strong;
};

/**
 * タスク種別・入力サイズから使うモデル ID を解決する。
 * tier 未設定 (cheap/strong とも無し・空) なら nullopt (backend 既定モデルに委ねる = 従来の単一 model 運用)。
 */
std::optional<std::string> resolve_tier_model(const lib::llm_gateway::RoutingInput& input, const std::optional<TierModels>& tiers);

using lib::llm_gateway::estimate_tokens;

} // namespace di::persona_engine
