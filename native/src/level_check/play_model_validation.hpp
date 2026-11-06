#pragma once
// Validate explicit joint outcomes and scenario boundaries before numeric calculation
// (src/level-check/play-model-validation.ts)。失敗は LevelCheckError("invalid_input", ...)。
#include <set>
#include <string>

#include "base/json.hpp"
#include "level_check/contracts.hpp"

namespace di::level_check {

inline constexpr double PROBABILITY_TOLERANCE = 1e-9;

/** references は参照できる文書 ID (level / baseline / references[].id)。 */
PlayModel validate_play_model(const Json& value, const std::set<std::string>& references);

} // namespace di::level_check
