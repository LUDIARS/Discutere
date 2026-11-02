#pragma once
// One rule/resource/MDA comparison; no level computation or discussion dependency (src/mechanics-check/tool.ts の移植)。
#include <chrono>
#include <optional>

#include "base/json.hpp"
#include "design_diagnostic/invoke.hpp"
#include "mechanics_check/contracts.hpp"

namespace di::mechanics_check {

using MechanicsCheckDependencies = design_diagnostic::DiagnosticDependencies;

/**
 * 要求を検証してから 1 回だけ LLM で比較する。playModel を持つ要求は LLM を呼ぶ前に use_level_check。
 * 失敗は MechanicsCheckError (invalid_input / use_level_check / llm_* / invalid_response / result_too_large)。
 */
MechanicsCheckResult check_mechanics_consistency(const Json& input, const MechanicsCheckDependencies& deps,
                                                 std::optional<std::chrono::milliseconds> timeout = std::nullopt);
/** validate_mechanics_check_request を通した要求を受ける版。 */
MechanicsCheckResult check_mechanics_consistency(const MechanicsCheckRequest& validated, const MechanicsCheckDependencies& deps,
                                                 std::optional<std::chrono::milliseconds> timeout = std::nullopt);

} // namespace di::mechanics_check
