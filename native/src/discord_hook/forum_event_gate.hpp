#pragma once
// @spec Di管理フォーラムの受信境界 (src/discord-hook/forum-event-gate.ts)。
//
// Gateway の受信イベント (MessageCreate / MessageReactionAdd / InteractionCreate / ThreadCreate) を、
// Di 管理フォーラム (guild ID と親 forum ID の組) の中のものだけハンドラへ渡す門。判定は
// managed_forum_scope (w1-D) と同じ: 空の scope は全拒否、未解決の親は読み取り fetch、失敗は拒否 (fail closed)。
// 加えて TS と同じく:
//   - bot の投稿・reply 投稿 (message.reference あり) は捨てる
//   - bot のリアクションは捨てる
//   - component の customId が別スレッドを指していたら (同じフォーラム内でも) 捨てる
//   - ThreadCreate は newlyCreated のときだけ
//   - ハンドラの例外は warn して握りつぶす (内容はログに出さない)
// discord.js の Client.on の代わりに、Gateway 担当がイベントを ForumEventGate の on_* に渡す。
// ManagedForumIds は Ready 後に解決されるので、照合のたびに provider から現在の値を読む。
#include <functional>
#include <optional>
#include <string>

#include "base/json.hpp"
#include "discord_hook/managed_forum_scope.hpp"

namespace di::discord_hook {

/** A component cannot operate another thread's pending state, even inside the same forum. */
bool component_matches_channel(const std::string& custom_id, const std::optional<std::string>& channel_id);

struct ForumMessageEvent {
    std::optional<std::string> guild_id;
    std::optional<std::string> channel_id;
    /** message.channel (手元に無ければ nullopt → fetch で確かめる)。 */
    std::optional<ForumChannelRef> channel;
    bool author_bot = false;
    /** message.reference がある (reply 投稿)。 */
    bool has_reference = false;
    /** ハンドラに渡す元の payload (Gateway の d)。 */
    Json raw = Json::object();
};

struct ForumReactionEvent {
    ForumMessageEvent message;
    bool user_bot = false;
    Json raw = Json::object();
};

struct ForumInteractionEvent {
    std::optional<std::string> guild_id;
    std::optional<std::string> channel_id;
    std::optional<ForumChannelRef> channel;
    /** component / modal の customId (slash command は nullopt)。 */
    std::optional<std::string> custom_id;
    Json raw = Json::object();
};

struct ForumThreadEvent {
    ForumChannelRef thread;
    bool newly_created = false;
    Json raw = Json::object();
};

struct ForumEventHandlers {
    std::function<void(const ForumMessageEvent&)> messageCreate;
    std::function<void(const ForumReactionEvent&)> reactionAdd;
    std::function<void(const ForumInteractionEvent&)> interactionCreate;
    /** 任意 (無ければ ThreadCreate は捨てる)。 */
    std::function<void(const ForumThreadEvent&, bool newly_created)> threadCreate;
};

using ManagedForumIdsProvider = std::function<ManagedForumIds()>;

class ForumEventGate {
public:
    ForumEventGate(ManagedForumIdsProvider forums, FetchForumChannel fetch_channel, ForumEventHandlers handlers);

    /** それぞれハンドラへ渡したら true (境界で捨てた・ハンドラが例外を出したら false)。 */
    bool on_message_create(const ForumMessageEvent& message) const;
    bool on_reaction_add(const ForumReactionEvent& reaction) const;
    bool on_interaction_create(const ForumInteractionEvent& interaction) const;
    bool on_thread_create(const ForumThreadEvent& thread) const;

private:
    bool managed(const ForumLocation& location) const;
    ManagedForumIdsProvider forums_;
    FetchForumChannel fetch_channel_;
    ForumEventHandlers handlers_;
};

/** The production gateway and fixtures bind the same guarded event listeners. */
ForumEventGate bind_managed_forum_events(ManagedForumIdsProvider forums, FetchForumChannel fetch_channel, ForumEventHandlers handlers);

} // namespace di::discord_hook
