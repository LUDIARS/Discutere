#pragma once
// 仮説の合成コマンド (src/core/projection/command-handlers/synthesis/index.ts)。
#include "core/projection/command_handlers/types.hpp"

namespace di::core::projection::command_handlers::synthesis {

HandlerResult handle_refs(const HandlerContext& ctx);
HandlerResult handle_propose(const HandlerContext& ctx);
HandlerResult handle_addresses(const HandlerContext& ctx);
HandlerResult handle_jump(const HandlerContext& ctx);
HandlerResult handle_submit(const HandlerContext& ctx);
HandlerResult handle_validate(const HandlerContext& ctx);
HandlerResult handle_integrate(const HandlerContext& ctx);
HandlerResult handle_reject(const HandlerContext& ctx);

} // namespace di::core::projection::command_handlers::synthesis
