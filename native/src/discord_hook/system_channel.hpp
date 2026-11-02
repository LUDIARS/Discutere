#pragma once
// システムカテゴリ配下の通知チャンネルを ensure する (id=61) — src/discord-hook/system-channel.ts の移植。
//
// 指定 guild の「システム」カテゴリ配下にチャンネルを用意する。無ければ作成する (Manage Channels 権限が必要)。
// 解決済みチャンネルは guild ごとに保持して毎回の検索/作成を避ける。
// discord.js のキャッシュ (guild.channels.cache) の代わりに GET /guilds/{id}/channels で探す。
#include <optional>
#include <string>

#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

struct EnsureSystemChannelOptions {
    std::string guildId;
    /** 親カテゴリ名 (既定「システム」) */
    std::optional<std::string> categoryName;
    /** チャンネル名 (既定「データ追加」) */
    std::optional<std::string> channelName;
    /** 新規作成時に付ける topic。既存チャンネル流用時は変更しない。 */
    std::optional<std::string> topic;
};

/** 「<category>/<channel>」を ensure して GuildText を返す。権限不足 / 取得失敗は nullopt。 */
std::optional<Channel> ensure_system_channel(DiscordApi& api, const EnsureSystemChannelOptions& opts);

/** Discord のチャンネル名の照合用 (小文字化・空白をハイフン化)。 */
std::string normalize_channel_name(const std::string& name);

/** テスト/再接続用にキャッシュをクリア。 */
void clear_system_channel_cache();

} // namespace di::discord_hook
