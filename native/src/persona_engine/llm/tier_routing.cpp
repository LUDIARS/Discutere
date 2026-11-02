#include "persona_engine/llm/tier_routing.hpp"

namespace di::persona_engine {

std::optional<std::string> resolve_tier_model(const lib::llm_gateway::RoutingInput& input, const std::optional<TierModels>& tiers) {
    auto truthy = [](const std::optional<std::string>& s) { return s && !s->empty(); };
    if (!tiers || (!truthy(tiers->cheap) && !truthy(tiers->strong))) return std::nullopt;
    const auto tier = lib::llm_gateway::pick_tier(input);
    // tiers.strong ?? tiers.cheap / tiers.cheap ?? tiers.strong (?? は値の有無だけを見る)
    if (tier == "strong") return tiers->strong ? tiers->strong : tiers->cheap;
    return tiers->cheap ? tiers->cheap : tiers->strong;
}

} // namespace di::persona_engine
