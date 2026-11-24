#pragma once

#include "playtest/contracts/runtime.hpp"

namespace di::flow::dialectic::checks {
inline const auto histrio_lookup = augur::post_only([](const auto& result, const auto& persona) {
    if (persona.role == "facilitator") return !result;
    if (persona.proxy)
        return persona.proxy->speakerKey.starts_with("ext:steam:")
            ? result == persona.proxy->histrioPersonaId : !result;
    return persona.poolPersonaId ? result == persona.debaterTemplateId : !result;
});
}
