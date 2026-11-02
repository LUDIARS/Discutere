#pragma once
// Discord の受信メッセージ (正規化後) — src/discord-hook/types.ts の移植。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::discord_hook {

struct DiscordInboundAuthor {
    std::string id;
    std::string username;
    std::optional<bool> bot;
};

struct DiscordInboundMention {
    std::optional<std::string> id;
    std::optional<std::string> username;
};

struct DiscordInboundMessage {
    std::string id;
    std::string channelId;
    DiscordInboundAuthor author;
    std::string content;
    std::vector<DiscordInboundMention> mentions;
    std::string timestamp;
};

Json to_json(const DiscordInboundAuthor& v);
Json to_json(const DiscordInboundMention& v);
Json to_json(const DiscordInboundMessage& v);

} // namespace di::discord_hook
