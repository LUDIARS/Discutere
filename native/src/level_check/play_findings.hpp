#pragma once
// Compose conditional numerical findings and explicitly missing model conditions (src/level-check/play-findings.ts)。
#include <vector>

#include "design_diagnostic/contracts.hpp"
#include "level_check/contracts.hpp"

namespace di::level_check {

struct PlayFindings {
    std::vector<PlayEnvelopeDifference> differences;
    std::vector<design_diagnostic::DesignGap> gaps;
};

PlayFindings compose_play_findings(const LevelCheckRequest& request, const std::vector<PlayEnvelope>& envelopes);

} // namespace di::level_check
