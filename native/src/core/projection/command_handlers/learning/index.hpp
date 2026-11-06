#pragma once
// mechanic 学習のコマンド (src/core/projection/command-handlers/learning/index.ts)。
#include "core/projection/command_handlers/types.hpp"

namespace di::core::projection::command_handlers::learning {

HandlerResult handle_define(const HandlerContext& ctx);
HandlerResult handle_intends(const HandlerContext& ctx);
HandlerResult handle_refine(const HandlerContext& ctx);
HandlerResult handle_compare(const HandlerContext& ctx);
HandlerResult handle_history(const HandlerContext& ctx);

} // namespace di::core::projection::command_handlers::learning
