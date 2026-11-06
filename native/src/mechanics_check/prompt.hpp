#pragma once
// Compare rules without attributing scenario/placement defects to mechanics (src/mechanics-check/prompt.ts)。
#include "design_diagnostic/invoke.hpp"
#include "mechanics_check/contracts.hpp"

namespace di::mechanics_check {

design_diagnostic::DiagnosticPrompt build_mechanics_check_prompt(const MechanicsCheckRequest& request);

} // namespace di::mechanics_check
