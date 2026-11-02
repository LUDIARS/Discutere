#pragma once
// Compare fixed scene conditions without confusing placement defects with rule defects (src/level-check/prompt.ts)。
#include "design_diagnostic/invoke.hpp"
#include "level_check/contracts.hpp"

namespace di::level_check {

design_diagnostic::DiagnosticPrompt build_level_check_prompt(const LevelCheckRequest& request);

} // namespace di::level_check
