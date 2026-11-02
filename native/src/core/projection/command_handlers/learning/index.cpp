#include "core/projection/command_handlers/learning/index.hpp"

#include "base/strings.hpp"
#include "core/events/event_types.hpp"

namespace di::core::projection::command_handlers::learning {

namespace {
constexpr sql::QueryLimits kHistoryRowLimits{100'000, 64u << 20};
} // namespace

HandlerResult handle_define(const HandlerContext& ctx) {
    const std::string name = str::trim(str::join(ctx.args, " "));
    if (name.empty()) return HandlerResult::failure("usage: /define <name>");
    repositories::MechanicInput input;
    input.workspaceId = ctx.workspaceId;
    input.name = name;
    const auto id = ctx.core->repos.mechanic.create(input);
    return HandlerResult::success("defined mechanic " + id);
}

HandlerResult handle_intends(const HandlerContext& ctx) {
    const std::string intends = str::trim(str::join(ctx.args, " "));
    if (intends.empty()) return HandlerResult::failure("usage: /intends <affect>");
    const auto row = ctx.core->raw().get("SELECT id,name,description FROM mechanics WHERE workspace_id = ? ORDER BY updated_at DESC LIMIT 1",
                                         {ctx.workspaceId});
    if (!row) return HandlerResult::failure("no mechanic to refine");
    Json payload = Json::object();
    payload["id"] = (*row)["id"].to_json();
    payload["workspaceId"] = ctx.workspaceId;
    payload["name"] = (*row)["name"].to_json();
    payload["description"] = (*row)["description"].to_json();  // NULL は null のまま (TS と同じ)
    payload["intends"] = intends;
    ctx.core->eventLog.append_event({std::nullopt, std::string(events::EVENT_TYPES::MechanicRefined), payload, std::nullopt});
    return HandlerResult::success("mechanic intent updated");
}

HandlerResult handle_refine(const HandlerContext& ctx) {
    const std::string description = str::trim(str::join(ctx.args, " "));
    if (description.empty()) return HandlerResult::failure("usage: /refine <text>");
    const auto row = ctx.core->raw().get("SELECT id,name,intends FROM mechanics WHERE workspace_id = ? ORDER BY updated_at DESC LIMIT 1",
                                         {ctx.workspaceId});
    if (!row) return HandlerResult::failure("no mechanic to refine");
    Json payload = Json::object();
    payload["id"] = (*row)["id"].to_json();
    payload["workspaceId"] = ctx.workspaceId;
    payload["name"] = (*row)["name"].to_json();
    payload["description"] = description;
    put_if_not_null(payload, "intends", (*row)["intends"]);
    ctx.core->eventLog.append_event({std::nullopt, std::string(events::EVENT_TYPES::MechanicRefined), payload, std::nullopt});
    return HandlerResult::success("mechanic refined");
}

HandlerResult handle_compare(const HandlerContext& ctx) {
    if (ctx.args.size() < 2) return HandlerResult::failure("usage: /compare <a> <b>");
    return HandlerResult::success("compare stub: " + ctx.args[0] + " vs " + ctx.args[1]);
}

HandlerResult handle_history(const HandlerContext& ctx) {
    const std::string name = str::trim(str::join(ctx.args, " "));
    if (name.empty()) return HandlerResult::failure("usage: /history <mechanic>");
    const auto rows = ctx.core->raw().all("SELECT id,description,intends FROM mechanics WHERE workspace_id = ? AND name = ? ORDER BY updated_at DESC",
                                          {ctx.workspaceId, name}, kHistoryRowLimits);
    return HandlerResult::success("history count=" + std::to_string(rows.size()));
}

} // namespace di::core::projection::command_handlers::learning
