#include "core/hypothesis/validation_routing.hpp"

#include "core/hypothesis/lifecycle_handler.hpp"

namespace di::core::hypothesis {

namespace {
/** 1 session の検証行の上限 (DB の値も外部入力)。 */
constexpr sql::QueryLimits kValidationRowLimits{100'000, 64u << 20};
} // namespace

int finalize_validation_for_session(projection::ReturnTypeCreateCore& core, const std::string& workspace_id, const std::string& session_id) {
    const auto rows = core.raw().all("SELECT DISTINCT hypothesis_id FROM hypothesis_validations WHERE workspace_id = ? AND session_id = ?",
                                     {workspace_id, session_id}, kValidationRowLimits);
    int moved = 0;
    for (const auto& r : rows) {
        const auto out = apply_lifecycle(core, r.str("hypothesis_id"), "session_end", {workspace_id, session_id});
        if (out.ok) moved += 1;
    }
    return moved;
}

} // namespace di::core::hypothesis
