#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "base/json.hpp"

namespace di::playtest {

inline constexpr std::size_t review_body_limit = 256u << 10;
inline constexpr std::string_view ai_review_heading = "AI オートプレイヤー由来の感想 (origin=ai / source=ai-autoplayer)";

struct AiReview {
    std::string runId;
    std::string gameId;
    std::string gameTitle;
    Json payload;
};

std::optional<AiReview> parse_ai_review(const Json& body, const std::string& key);
std::string format_ai_review(const Json& review);

} // namespace di::playtest
