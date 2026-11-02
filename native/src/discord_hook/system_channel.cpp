#include "discord_hook/system_channel.hpp"

#include <map>
#include <mutex>

#include "base/strings.hpp"

namespace di::discord_hook {

namespace {
std::mutex& cache_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, Channel>& cache() {
    static std::map<std::string, Channel> c;
    return c;
}
} // namespace

std::string normalize_channel_name(const std::string& name) {
    const std::string lower = str::to_lower(name);
    std::string out;
    std::size_t i = 0;
    bool in_space = false;
    while (i < lower.size()) {
        const std::size_t start = i;
        const auto cp = str::next_codepoint(lower, i);
        if (str::is_js_space(cp)) {
            if (!in_space) out.push_back('-');
            in_space = true;
            continue;
        }
        in_space = false;
        out.append(lower, start, i - start);
    }
    return out;
}

std::optional<Channel> ensure_system_channel(DiscordApi& api, const EnsureSystemChannelOptions& opts) {
    const std::string category_name = opts.categoryName.value_or("システム");
    const std::string channel_name = opts.channelName.value_or("データ追加");
    const std::string key = opts.guildId + ":" + category_name + "/" + channel_name;
    {
        std::lock_guard lock(cache_mutex());
        auto it = cache().find(key);
        if (it != cache().end()) return it->second;
    }
    try {
        api.get_guild(opts.guildId);
        // Bot に Manage Channels が無ければ作成できない → 既存チャンネルだけ探す。
        const bool can_manage = (api.current_member_permissions(opts.guildId) & permission::ManageChannels) != 0;

        const auto channels = api.get_guild_channels(opts.guildId);
        const std::string target = normalize_channel_name(channel_name);
        for (const auto& c : channels) {
            if (c.type == channel_type::GuildText && normalize_channel_name(c.name.value_or("")) == target) {
                std::lock_guard lock(cache_mutex());
                cache()[key] = c;
                return c;
            }
        }
        if (!can_manage) return std::nullopt;

        std::optional<Channel> category;
        for (const auto& c : channels)
            if (c.type == channel_type::GuildCategory && c.name.value_or("") == category_name) {
                category = c;
                break;
            }
        if (!category) category = api.create_guild_channel(opts.guildId, Json{{"name", category_name}, {"type", channel_type::GuildCategory}});

        const auto created = api.create_guild_channel(
            opts.guildId, Json{{"name", channel_name},
                               {"type", channel_type::GuildText},
                               {"parent_id", category->id},
                               {"topic", opts.topic.value_or("外部議論データの取り込み通知 (Discutere クロール)")}});
        std::lock_guard lock(cache_mutex());
        cache()[key] = created;
        return created;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void clear_system_channel_cache() {
    std::lock_guard lock(cache_mutex());
    cache().clear();
}

} // namespace di::discord_hook
