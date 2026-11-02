#pragma once
// Bounded level documents and optional explicit joint-outcome model (src/level-check/request.ts)。
#include "base/json.hpp"
#include "design_diagnostic/documents.hpp"
#include "level_check/contracts.hpp"

namespace di::level_check {

/** 失敗は LevelCheckError("invalid_input", ...)。 */
LevelCheckRequest validate_level_check_request(const Json& value);

/** level / baseline / references に、模型があれば "playModel" (model_assumption) を足した文書表。 */
design_diagnostic::DocumentMap supplied_level_documents(const LevelCheckRequest& request);

} // namespace di::level_check
