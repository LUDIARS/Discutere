#include "core/projection/command_handlers/cross/stale.hpp"

#include "base/strings.hpp"

namespace di::core::projection::command_handlers::cross {

namespace {
constexpr sql::QueryLimits kStaleRowLimits{100'000, 64u << 20};
} // namespace

HandlerResult handle_stale(const HandlerContext& ctx) {
    const auto rows = ctx.core->raw().all(
        "SELECT id,statement,status FROM hypotheses WHERE workspace_id = ? AND stale_flagged = 1 ORDER BY updated_at DESC", {ctx.workspaceId},
        kStaleRowLimits);
    if (rows.empty()) return HandlerResult::success("no stale hypotheses");
    std::vector<std::string> lines;
    for (const auto& r : rows)
        lines.push_back("- " + js_template(r["id"]) + " (" + js_template(r["status"]) + "): " + js_template(r["statement"]));
    return HandlerResult::success(str::join(lines, "\n"));
}

} // namespace di::core::projection::command_handlers::cross
