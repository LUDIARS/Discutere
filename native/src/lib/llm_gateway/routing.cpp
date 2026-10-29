#include "lib/llm_gateway/routing.hpp"

#include <algorithm>

namespace di::lib::llm_gateway {

Tier pick_tier(const RoutingInput& input, const std::optional<RoutingRules>& rules) {
    if (input.forceTier) return *input.forceTier;
    // DEFAULT_RULES に rules を上書き合成する ({ ...DEFAULT_RULES, ...rules })。
    static const std::vector<std::string> kDefaultStrongKinds = {"converge", "facilitate", "aufhebung", "synthesize"};
    constexpr double kDefaultStrongAboveTokens = 8000;
    const std::vector<std::string>& strong_kinds =
        rules && rules->strongKinds ? *rules->strongKinds : kDefaultStrongKinds;
    const double strong_above = rules && rules->strongAboveTokens ? *rules->strongAboveTokens : kDefaultStrongAboveTokens;
    if (std::find(strong_kinds.begin(), strong_kinds.end(), input.kind) != strong_kinds.end()) return "strong";
    if (input.inputTokens && *input.inputTokens > strong_above) return "strong";
    return "cheap";
}

} // namespace di::lib::llm_gateway
