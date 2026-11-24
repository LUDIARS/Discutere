#pragma once

#include "base/json.hpp"
#include "playtest/contracts/runtime.hpp"

namespace di::integrations::checks {
inline const auto histrio_profile = augur::post_only([](const auto& result, const auto& body, const auto& persona_id) {
    if (!result) return true;
    return di::json_string(body, "id") == persona_id &&
           di::json_string(body, "script_pattern_id") == result->patternId && !result->patternId.empty() &&
           di::json_string(body, "script_pattern_version") == result->patternVersion && !result->patternVersion.empty();
});
}
