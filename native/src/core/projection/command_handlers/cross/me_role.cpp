#include "core/projection/command_handlers/cross/me_role.hpp"

#include "base/strings.hpp"

namespace di::core::projection::command_handlers::cross {

HandlerResult handle_me_role(const HandlerContext& ctx) {
    const std::string role = str::to_lower(ctx.args.empty() ? std::string() : ctx.args[0]);
    if (!ctx.personId || ctx.personId->empty()) return HandlerResult::failure("personId is required");
    if (role != "theorist" && role != "player" && role != "developer")
        return HandlerResult::failure("usage: /me role <theorist|player|developer>");

    auto& repo = ctx.core->repos.person;
    if (const auto p = repo.get(*ctx.personId)) {
        repo.update(*ctx.personId, {ctx.workspaceId, p->str("name"), role});
    } else {
        repo.create({*ctx.personId, ctx.workspaceId, *ctx.personId, role});
    }
    return HandlerResult::success("role updated to " + role);
}

} // namespace di::core::projection::command_handlers::cross
