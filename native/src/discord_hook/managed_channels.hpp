#pragma once
// Di が運用上 自動作成する専用チャンネル (フォーラム集約) — src/discord-hook/managed-channels.ts の移植。
// 起動時に guild ごとに「データ学習依頼」「まとめ投稿」を ensure する (実体は system_channel)。
#include <string>
#include <vector>

#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

struct ManagedChannelNames {
    /** データ学習依頼チャンネル名 (既定「データ学習依頼」) */
    std::string dataLearning;
    /** まとめ投稿チャンネル名 (既定「まとめ投稿」) */
    std::string summary;
    /** 親カテゴリ名 (既定「システム」) */
    std::string category;
};
const ManagedChannelNames& default_managed_channel_names();

struct EnsureManagedChannelsResult {
    /** クロール入口として監視に足すべきチャンネル id 群 (データ学習依頼)。 */
    std::vector<std::string> dataLearningChannelIds;
};

EnsureManagedChannelsResult ensure_managed_channels(DiscordApi& api, const std::vector<std::string>& guild_ids,
                                                    const ManagedChannelNames& names = default_managed_channel_names());

} // namespace di::discord_hook
