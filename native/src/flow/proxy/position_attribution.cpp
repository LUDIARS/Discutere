#include "flow/proxy/position_attribution.hpp"

#include "flow/proxy/attribution.hpp"

namespace di::flow::proxy {

dialectic::GeneratedPosition attribute_position_grounds(const dialectic::GeneratedPosition& gp, const FlowPersona& persona,
                                                        const WarnFn& warn) {
    if (!persona.proxy) return gp;
    AttributionScope scope;
    scope.evidenceIds = persona.proxy->evidenceIds;
    for (const auto& [id, text] : persona.proxy->evidenceTexts) scope.evidenceTexts.emplace(id, text);
    dialectic::GeneratedPosition out = gp;
    for (auto& g : out.grounds) {
        CheckAttributionArgs a;
        a.claimKind = g.claimKind;
        a.evidenceIds = g.evidenceIds.value_or(std::vector<std::string>{});
        a.data = g.text;
        a.scope = scope;
        const auto result = check_attribution(a);
        if (!result.ok) {
            if (warn)
                warn("代理 " + persona.name + " の論拠 " + g.id + ": 帰属の検査に通らない (" + result.foul.value_or("") +
                     ") — 推測として扱う");
            g.claimKind = "simulationInference";
        } else {
            g.claimKind = result.claimKind;
        }
        g.evidenceIds = result.evidenceIds;
    }
    for (std::size_t i = 0; i < out.groundTexts.size(); ++i)
        if (i < out.grounds.size())
            out.groundTexts[i] += attribution_suffix(*out.grounds[i].claimKind, *out.grounds[i].evidenceIds);
    return out;
}

} // namespace di::flow::proxy
