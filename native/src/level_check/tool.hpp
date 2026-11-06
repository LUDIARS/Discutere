#pragma once
// One level comparison plus pure conditional envelope arithmetic; no mechanics-use-case dependency
// (src/level-check/tool.ts の移植)。
#include <chrono>
#include <optional>

#include "base/json.hpp"
#include "design_diagnostic/invoke.hpp"
#include "level_check/contracts.hpp"

namespace di::level_check {

using LevelCheckDependencies = design_diagnostic::DiagnosticDependencies;

/**
 * 要求を検証してから 1 回だけ LLM で比較し、到達幅 (数値) の所見を足す。
 * 失敗は LevelCheckError (invalid_input / llm_* / invalid_response / result_too_large)。
 * timeout は invoke_diagnostic に渡す (省略時 DIAGNOSTIC_LIMITS.timeoutMs、テスト用に短くできる)。
 */
LevelCheckResult check_level_consistency(const Json& input, const LevelCheckDependencies& deps,
                                         std::optional<std::chrono::milliseconds> timeout = std::nullopt);
/** validate_level_check_request を通した要求を受ける版 (検証は冪等なので省く)。 */
LevelCheckResult check_level_consistency(const LevelCheckRequest& validated, const LevelCheckDependencies& deps,
                                         std::optional<std::chrono::milliseconds> timeout = std::nullopt);

} // namespace di::level_check
