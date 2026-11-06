#pragma once
// LLM supplies only level comparisons; play_envelope is never accepted here (src/level-check/response.ts)。
#include <string>

#include "design_diagnostic/contracts.hpp"
#include "level_check/contracts.hpp"

namespace di::level_check {

/** 失敗は LevelCheckError("invalid_response", ...)。 */
design_diagnostic::DiagnosticResult parse_level_check_response(const std::string& text, const LevelCheckRequest& request);

} // namespace di::level_check
