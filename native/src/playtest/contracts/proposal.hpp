#pragma once
#include "playtest/contracts/runtime.hpp"

namespace di::playtest::checks {
inline const auto proposal = augur::post_only([](const auto& result, const auto& id, const auto& project, const auto& target,
                                                 const auto& summary, auto observed_at) {
    return result.size() == 8 && result.at("event_key") == "di:proposal:" + id && result.at("source") == "di" &&
           result.at("kind") == "improvement_proposal" && result.at("origin") == "ai" &&
           result.at("project_code") == project && result.at("target_key") == target &&
           result.at("summary") == summary && result.at("observed_at") == observed_at;
});
}
