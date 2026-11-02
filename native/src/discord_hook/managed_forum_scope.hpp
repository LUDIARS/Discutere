#pragma once
// @spec Di管理フォーラムの受信境界 (spec/feature/discord-own-forum-only.md) — src/discord-hook/managed-forum-scope.ts の移植。
//
// guild ごとに解決した Di 管理フォーラムの ID を親とするスレッドだけを受ける。フォーラムの名前や
// GuildForum 型だけでは許可せず、guild ID と親 forum ID の組を照合する。未設定 (空の scope) は全拒否。
// 未解決の親は読み取り fetch で ID / guild / type を確かめ、取得失敗は拒否 (fail closed)。
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

/** guildId → 管理フォーラム ID。 */
using ManagedForumIds = std::map<std::string, std::string>;

/** 照合に使うチャンネルの同一性 (discord.js のチャンネルオブジェクトの必要部分)。 */
struct ForumChannelRef {
    std::optional<std::string> id;
    std::optional<std::string> guild_id;
    std::optional<std::string> parent_id;
    std::optional<int> type;
    /** channel.isThread() */
    bool is_thread = false;
    /** キャッシュ済みの親 (無ければ nullptr = uncached)。 */
    std::shared_ptr<const ForumChannelRef> parent;
};

/** REST の Channel から作る (is_thread は type から、parent は持たない)。 */
ForumChannelRef forum_ref_from_channel(const Channel& channel);

/** Identity, not the forum's name or type alone, grants access. Empty scope denies all. */
bool is_managed_forum_thread(const ForumChannelRef* channel, const ManagedForumIds& forums);

struct ForumLocation {
    std::optional<std::string> guild_id;
    std::optional<std::string> channel_id;
    /** 手元にあるチャンネル (無ければ fetch する)。 */
    std::optional<ForumChannelRef> channel;
};

/** チャンネルを読み取りで取得する (見つからなければ nullopt、失敗は例外でよい)。 */
using FetchForumChannel = std::function<std::optional<ForumChannelRef>(const std::string& channel_id)>;

/** Fetch only unresolved identities in a configured guild, and fail closed on any error. */
bool is_managed_forum_location(const ForumLocation& location, const ManagedForumIds& forums, const FetchForumChannel& fetch_channel);

/** DiscordApi::get_channel で取得する FetchForumChannel。 */
FetchForumChannel rest_forum_channel_fetcher(DiscordApi& api);

} // namespace di::discord_hook
