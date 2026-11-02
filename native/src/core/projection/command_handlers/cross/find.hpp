#pragma once
// /find (src/core/projection/command-handlers/cross/find.ts) — mechanic / affect / hypothesis / utterance の部分一致検索。
#include "core/projection/command_handlers/types.hpp"

namespace di::core::projection::command_handlers::cross {

HandlerResult handle_find(const HandlerContext& ctx);

} // namespace di::core::projection::command_handlers::cross
