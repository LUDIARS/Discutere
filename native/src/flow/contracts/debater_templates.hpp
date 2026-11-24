#pragma once

#include <algorithm>
#include <set>
#include <string>

#include "playtest/contracts/runtime.hpp"

namespace di::flow::checks {
inline const auto debater_templates = augur::post_only([](const auto& cast, const auto&) {
    std::set<std::string> ids;
    std::size_t eligible = 0;
    for (const auto& persona : cast) {
        if (persona.role == "facilitator" || persona.proxy || !persona.poolPersonaId) {
            if (persona.debaterTemplateId) return false;
            continue;
        }
        ++eligible;
        if (!persona.debaterTemplateId || !persona.debaterTemplateId->starts_with("tpl-")) return false;
        ids.insert(*persona.debaterTemplateId);
    }
    return ids.size() == std::min(eligible, std::size_t{12});
});
}
