#pragma once
// 感情検証モードのコマンド (src/core/projection/command-handlers/emotion/index.ts)。
#include "core/projection/command_handlers/types.hpp"

namespace di::core::projection::command_handlers::emotion {

/** /scene <description> — session を emotion モードにして scene を設定する。 */
HandlerResult handle_scene(const HandlerContext& ctx);
/** /done — session を終え、検証中の仮説を確定させる。 */
HandlerResult handle_done(const HandlerContext& ctx);

} // namespace di::core::projection::command_handlers::emotion
