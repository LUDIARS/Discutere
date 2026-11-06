#include "discord_hook/managed_channels.hpp"

#include "base/log.hpp"
#include "discord_hook/system_channel.hpp"

namespace di::discord_hook {

const ManagedChannelNames& default_managed_channel_names() {
    static const ManagedChannelNames n{"データ学習依頼", "まとめ投稿", "システム"};
    return n;
}

EnsureManagedChannelsResult ensure_managed_channels(DiscordApi& api, const std::vector<std::string>& guild_ids, const ManagedChannelNames& names) {
    EnsureManagedChannelsResult out;
    for (const auto& guild_id : guild_ids) {
        auto data_learning = ensure_system_channel(
            api, {guild_id, names.category, names.dataLearning, std::string("学習させたいデータの URL を貼ってください (Discutere クロール依頼)")});
        if (data_learning) {
            out.dataLearningChannelIds.push_back(data_learning->id);
            log::info("  managed-channel: データ学習依頼 = #" + data_learning->name.value_or("") + " (" + data_learning->id + ")");
        }
        // まとめ投稿は存在保証だけ (転記時に finalizeForumPost が再 ensure する)。
        auto summary = ensure_system_channel(
            api, {guild_id, names.category, names.summary, std::string("収束した議論の結論まとめ (Discutere フォーラム議論)")});
        if (summary) log::info("  managed-channel: まとめ投稿 = #" + summary->name.value_or("") + " (" + summary->id + ")");
    }
    return out;
}

} // namespace di::discord_hook
