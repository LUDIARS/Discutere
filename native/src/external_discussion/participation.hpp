#pragma once
// 参加機会の判定。移植元 src/external-discussion/participation.ts。
#include <cstdint>
#include <optional>
#include <string>

#include "external_discussion/contracts.hpp"

namespace di::external_discussion {

inline constexpr std::int64_t SILENCE_MS = 180'000;

struct Opportunity {
    std::string id;
    std::string humanId;
    std::int64_t humanAt = 0;
    std::string mode;  // "quiet" | "active"
};

/** 新しい人間の発言だけが参加の機会を作る。ポーリング頻度が確率に影響しないこと。 */
std::optional<Opportunity> opportunity(const ConversationInput& input, std::int64_t now);

} // namespace di::external_discussion
