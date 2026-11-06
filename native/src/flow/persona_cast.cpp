#include "flow/persona_cast.hpp"

#include <algorithm>
#include <set>

#include "base/strings.hpp"
#include "flow/persona_pool.hpp"

namespace di::flow {
namespace {

std::vector<std::string> normalize_persona_ids(const std::vector<std::string>& persona_ids) {
    std::set<std::string> seen;
    std::vector<std::string> out;
    for (const auto& raw : persona_ids) {
        const std::string id = str::trim(raw);
        if (id.empty() || seen.count(id)) continue;
        seen.insert(id);
        out.push_back(id);
    }
    return out;
}

FlowRole specified_role(const FlowRole& role) { return role == "facilitator" ? "opinion" : role; }

std::optional<FlowStance> specified_stance(const FlowRole& role, int debater_index) {
    if (role != "debater") return std::nullopt;
    return debater_index % 2 == 0 ? "pro" : "con";
}

} // namespace

std::vector<FlowPersona> build_flow_cast(const BuildFlowCastArgs& args) {
    GeneratePersonasArgs g;
    g.count = args.count;
    g.defaultModel = args.defaultModel;
    g.isLocal = args.isLocal;
    g.rng = args.rng;
    g.roster = args.roster;
    const auto generated = generate_flow_personas(g);
    const FlowPersona& facilitator = generated[0];
    std::vector<FlowPersona> specified;
    std::set<std::string> seen_pool_ids;
    int debater_index = 0;

    for (const auto& id_or_name : normalize_persona_ids(args.personaIds)) {
        auto pool_persona = find_pool_persona(id_or_name);
        if (!pool_persona) {
            if (args.warn) args.warn("生成済みペルソナが見つかりません: " + id_or_name);
            continue;
        }
        if (seen_pool_ids.count(pool_persona->id)) continue;
        seen_pool_ids.insert(pool_persona->id);

        const FlowRole role = specified_role(pool_persona->role);
        const auto stance = specified_stance(role, debater_index);
        if (role == "debater") debater_index += 1;
        specified.push_back(to_flow_persona(*pool_persona, {role, args.defaultModel, args.isLocal, stance}));
    }

    const std::size_t target = std::max<std::size_t>(static_cast<std::size_t>(std::max(args.count, 0)), 1 + specified.size());
    std::set<std::string> used_ids{facilitator.id}, used_names{facilitator.name};
    for (const auto& p : specified) {
        used_ids.insert(p.id);
        used_names.insert(p.name);
    }
    std::vector<FlowPersona> fillers;
    if (1 + specified.size() < target) {
        for (std::size_t i = 1; i < generated.size(); ++i) {
            const auto& persona = generated[i];
            if (used_ids.count(persona.id) || used_names.count(persona.name)) continue;
            fillers.push_back(persona);
            used_ids.insert(persona.id);
            used_names.insert(persona.name);
            if (1 + specified.size() + fillers.size() >= target) break;
        }
    }

    std::vector<FlowPersona> out{facilitator};
    out.insert(out.end(), specified.begin(), specified.end());
    out.insert(out.end(), fillers.begin(), fillers.end());
    return out;
}

} // namespace di::flow
