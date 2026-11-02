#pragma once
// ゲーム感想チャンネルの ensure / 抽出 — src/discord-hook/game-feedback-channel.ts の移植。
//
// - カテゴリ「ゲーム感想」を起動時に ensure (無ければ作成、Manage Channels 権限が要る)。
// - そのカテゴリ配下のチャンネルへの投稿は、議論にせず感想 (意見データ) として収集する。
// gameTitle はチャンネル名そのまま (Discord が正規化した slug)。投稿者は含めない (匿名)。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

/** 各 guild に感想カテゴリを ensure (無ければ作成)。失敗は warn して skip。 */
void ensure_game_feedback_category(DiscordApi& api, const std::vector<std::string>& guild_ids, const std::string& category_name);

struct ExtractedFeedback {
    std::string gameTitle;
    std::string content;
    double createdAt = 0;
};

/**
 * メッセージが感想カテゴリ配下のチャンネル (GuildText / GuildAnnouncement) への投稿なら感想として抽出する。
 * channel はメッセージのチャンネル、parent はその親 (無ければ nullopt)。該当しなければ nullopt。
 */
std::optional<ExtractedFeedback> extract_game_feedback(const Message& msg, const Channel& channel, const std::optional<Channel>& parent,
                                                       const std::string& category_name);

} // namespace di::discord_hook
