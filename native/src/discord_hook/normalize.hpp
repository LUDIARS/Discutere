#pragma once
// Discord 受信 payload の正規化 (src/discord-hook/normalize.ts の移植)。
// 3 つの形を受ける: 生メッセージ / Gateway {t:"MESSAGE_CREATE", d} / {event: {...}}。
// payload は呼び出し側が上限つきで読んだ JSON (parse_json) であること。入れ子の event は深さ 16 までたどる。
#include <optional>

#include "base/json.hpp"
#include "discord_hook/types.hpp"

namespace di::discord_hook {

std::optional<DiscordInboundMessage> normalize_discord_inbound_message(const Json& payload);

} // namespace di::discord_hook
