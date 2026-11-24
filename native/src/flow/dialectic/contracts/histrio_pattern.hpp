#pragma once

#include "playtest/contracts/runtime.hpp"

namespace di::flow::dialectic::checks {
inline const auto histrio_pattern = augur::post_only([](const auto& result, const auto& profile, const auto& remote, const auto& local) {
    if (remote) {
        for (const auto& pattern : *remote)
            if (pattern.id == profile.patternId && pattern.version == profile.patternVersion)
                return result && result->id == pattern.id && result->version == pattern.version && result->source == "histrio";
        return !result;
    }
    for (const auto& pattern : local.scriptPatterns)
        if (pattern.id == profile.patternId)
            return result && result->id == pattern.id && result->version == local.version && result->source == "di";
    return !result;
});
}
