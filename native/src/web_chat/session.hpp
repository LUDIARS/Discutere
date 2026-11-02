#pragma once
// src/web-chat/session.ts の移植: Web チャット トランスポートの session 管理 (Discord の ensureDiscordSession の web 版)。
//
// Di の議論は `scene` 文字列でトランスポートを束ねる。Discord は `discord:<guild>/<channel>`、Web チャットは
// `web:<roomId>`。入口の人間発話 session・進行中議論 session ともに同じ scene を持たせ、1 つの room を
// 1 つの会話として読み書きする (transcript)。
// 個人データは保存しない (匿名 workspace 方針)。author は表示名のみで personId に載せる。
#include <string>
#include <string_view>

#include "core/index.hpp"

namespace di::web_chat {

/** web room に対応する scene 文字列。 */
std::string web_scene(std::string_view room_id);

/** roomId を session/scene に使える安全な文字列に正規化する (英数 . _ - のみ、32 文字。空なら "lobby")。 */
std::string sanitize_room_id(std::string_view raw);

/** web room の入口 session を 1 つ確保する (なければ作る)。 */
std::string ensure_web_session(core::Core& core, const std::string& workspace_id, const std::string& room_id);

/** 当該 room (scene=web:<roomId>) で open な議論 session が既にあるか。 */
bool web_discussion_exists(core::Core& core, const std::string& workspace_id, const std::string& room_id);

} // namespace di::web_chat
