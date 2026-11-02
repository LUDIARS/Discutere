#include "discord_hook/game_feedback_channel.hpp"

#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"

namespace di::discord_hook {

void ensure_game_feedback_category(DiscordApi& api, const std::vector<std::string>& guild_ids, const std::string& category_name) {
    for (const auto& guild_id : guild_ids) {
        try {
            api.get_guild(guild_id);
            std::optional<Channel> existing;
            for (const auto& c : api.get_guild_channels(guild_id))
                if (c.type == channel_type::GuildCategory && c.name.value_or("") == category_name) {
                    existing = c;
                    break;
                }
            if (existing) {
                log::info("  game-feedback: category #" + category_name + " ok (" + existing->id + ")");
                continue;
            }
            const auto created = api.create_guild_channel(guild_id, Json{{"name", category_name}, {"type", channel_type::GuildCategory}});
            log::info("  game-feedback: created category #" + category_name + " (" + created.id + ")");
        } catch (const std::exception& e) {
            log::warn("  game-feedback: ensure category failed (" + guild_id + "): " + e.what());
        }
    }
}

std::optional<ExtractedFeedback> extract_game_feedback(const Message& msg, const Channel& channel, const std::optional<Channel>& parent,
                                                       const std::string& category_name) {
    // 通常のギルドテキスト/announcement チャンネルのみ対象 (スレッドやDMは除外)。
    if (channel.type != channel_type::GuildText && channel.type != channel_type::GuildAnnouncement) return std::nullopt;
    if (!parent || parent->type != channel_type::GuildCategory || parent->name.value_or("") != category_name) return std::nullopt;
    if (channel.parent_id && *channel.parent_id != parent->id) return std::nullopt;  // 親の取り違えを防ぐ (移植で追加)
    const std::string content = str::trim(msg.content);
    if (content.empty()) return std::nullopt;
    const auto ts = time::parse_iso(msg.timestamp);
    return ExtractedFeedback{channel.name.value_or(""), content, ts ? static_cast<double>(*ts) : 0.0};
}

} // namespace di::discord_hook
