#pragma once
// Pure arithmetic over supplied joint outcomes; never synthesizes combinations (src/level-check/play-envelope.ts)。
#include <vector>

#include "level_check/contracts.hpp"

namespace di::level_check {

/** The model must have passed validate_level_check_request at the caller boundary. 桁あふれは LevelCheckError("invalid_input")。 */
std::vector<PlayEnvelope> calculate_play_envelopes(const PlayModel& model);

} // namespace di::level_check
