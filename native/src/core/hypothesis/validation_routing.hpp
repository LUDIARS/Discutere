#pragma once
// session 終了時に検証中の仮説を確定させる (src/core/hypothesis/validation-routing.ts)。
#include <string>

#include "core/projection/types.hpp"

namespace di::core::hypothesis {

/** session で検証された仮説に session_end を適用し、遷移できた件数を返す。 */
int finalize_validation_for_session(projection::ReturnTypeCreateCore& core, const std::string& workspace_id, const std::string& session_id);

} // namespace di::core::hypothesis
