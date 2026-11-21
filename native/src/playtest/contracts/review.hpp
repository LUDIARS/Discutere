#pragma once
#include "playtest/contracts/runtime.hpp"

namespace di::playtest::checks {
inline const auto review = augur::post_only([](const auto& result, const auto& body, const auto& key) {
    if (!result) return true;
    return body.size() == 11 && body.at("origin") == "ai" && body.at("source") == "ai-autoplayer" &&
           body.at("run_id").template get<std::string>() == key;
});
}
