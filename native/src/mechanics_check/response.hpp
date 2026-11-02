#pragma once
// Feature-specific kind whitelist over shared citation/comparison validation (src/mechanics-check/response.ts)。
#include <string>

#include "mechanics_check/contracts.hpp"

namespace di::mechanics_check {

/** 失敗は MechanicsCheckError("invalid_response", ...)。 */
MechanicsCheckResult parse_mechanics_check_response(const std::string& text, const MechanicsCheckRequest& request);

} // namespace di::mechanics_check
