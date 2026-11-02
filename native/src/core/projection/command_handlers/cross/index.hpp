#pragma once
// 横断コマンドの振り分け (src/core/projection/command-handlers/cross/index.ts)。
#include <string>

#include "core/projection/command_handlers/types.hpp"

namespace di::core::projection::command_handlers::cross {

HandlerResult handle_cross(const HandlerContext& ctx, const std::string& command);

} // namespace di::core::projection::command_handlers::cross
