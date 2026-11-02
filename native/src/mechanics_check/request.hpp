#pragma once
// Strict mechanics documents; reject numeric models rather than silently forwarding them (src/mechanics-check/request.ts)。
#include "base/json.hpp"
#include "design_diagnostic/documents.hpp"
#include "mechanics_check/contracts.hpp"

namespace di::mechanics_check {

/** playModel を持つ要求は MechanicsCheckError("use_level_check")、その他の不正は ("invalid_input")。 */
MechanicsCheckRequest validate_mechanics_check_request(const Json& value);

design_diagnostic::DocumentMap supplied_documents(const MechanicsCheckRequest& request);

} // namespace di::mechanics_check
