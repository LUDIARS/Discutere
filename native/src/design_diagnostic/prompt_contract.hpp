#pragma once
// Common structured comparison and source-use instructions (src/design-diagnostic/prompt-contract.ts)。
#include <string>

namespace di::design_diagnostic {

/** DIAGNOSTIC_PROMPT_CONTRACT (各配列の件数は DIAGNOSTIC_LIMITS.findingsPerKind)。 */
const std::string& diagnostic_prompt_contract();

} // namespace di::design_diagnostic
