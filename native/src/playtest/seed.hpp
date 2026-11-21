#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "discussion_seed/seed.hpp"

namespace di::playtest {

struct VoiceSeedPolicy {
    std::int64_t minNewVoices = 3;
    std::int64_t cooldownMs = 86'400'000;
};

struct PlaytestSeed {
    std::string gapId;
    std::string title;
};

bool voice_threshold_met(std::int64_t count, std::int64_t minimum);
std::optional<PlaytestSeed> seed_from_playtest_voices(
    sql::Database& voices, const discussion_seed::SeedHeadlessDiscussionArgs& base, const VoiceSeedPolicy& policy, std::int64_t now);

} // namespace di::playtest
