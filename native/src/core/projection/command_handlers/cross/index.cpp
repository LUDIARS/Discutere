#include "core/projection/command_handlers/cross/index.hpp"

#include "base/strings.hpp"
#include "core/projection/command_handlers/cross/cluster.hpp"
#include "core/projection/command_handlers/cross/find.hpp"
#include "core/projection/command_handlers/cross/hotspot.hpp"
#include "core/projection/command_handlers/cross/lineage.hpp"
#include "core/projection/command_handlers/cross/me_role.hpp"
#include "core/projection/command_handlers/cross/stale.hpp"

namespace di::core::projection::command_handlers::cross {

HandlerResult handle_cross(const HandlerContext& ctx, const std::string& command) {
    if (command == "me") {
        if (str::to_lower(ctx.args.empty() ? std::string() : ctx.args[0]) != "role") return HandlerResult::failure("usage: /me role <...>");
        HandlerContext next = ctx;
        next.args.erase(next.args.begin());
        return handle_me_role(next);
    }
    if (command == "find") return handle_find(ctx);
    if (command == "lineage") return handle_lineage(ctx);
    if (command == "cluster") return HandlerResult::success(handle_cluster(*ctx.core, ctx.workspaceId));
    if (command == "stale") return handle_stale(ctx);
    if (command == "hotspot") return HandlerResult::success(handle_hotspot(*ctx.core, ctx.workspaceId));
    return HandlerResult::failure("unknown cross command: " + command);
}

} // namespace di::core::projection::command_handlers::cross
