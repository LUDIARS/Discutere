#include "core/projection/command_handlers/cross/lineage.hpp"

#include "base/strings.hpp"

namespace di::core::projection::command_handlers::cross {

HandlerResult handle_lineage(const HandlerContext& ctx) {
    if (ctx.args.empty() || ctx.args[0].empty()) return HandlerResult::failure("usage: /lineage <mechanic_id>");
    const std::string& mid = ctx.args[0];
    auto& raw = ctx.core->raw();
    const auto m = raw.get("SELECT id,name,description FROM mechanics WHERE id = ?", {mid});
    if (!m) return HandlerResult::failure("mechanic not found");
    const std::string like = "%" + js_template((*m)["name"]) + "%";
    const auto utterances = raw.all(
        "SELECT id,raw_content,session_id FROM utterances WHERE workspace_id = ? AND raw_content LIKE ? ORDER BY created_at DESC LIMIT 10",
        {ctx.workspaceId, like});
    const auto hyps = raw.all(
        "SELECT id,statement,status FROM hypotheses WHERE workspace_id = ? AND statement LIKE ? ORDER BY updated_at DESC LIMIT 10",
        {ctx.workspaceId, like});
    const std::vector<std::string> lines{
        "# Lineage " + js_template((*m)["name"]),
        "- mechanic: " + js_template((*m)["id"]),
        "- utterances: " + std::to_string(utterances.size()),
        "- hypotheses: " + std::to_string(hyps.size()),
    };
    return HandlerResult::success(str::join(lines, "\n"));
}

} // namespace di::core::projection::command_handlers::cross
