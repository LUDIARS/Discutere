#include "discord_hook/managed_forum_scope.hpp"

namespace di::discord_hook {

ForumChannelRef forum_ref_from_channel(const Channel& channel) {
    ForumChannelRef r;
    r.id = channel.id;
    r.guild_id = channel.guild_id;
    r.parent_id = channel.parent_id;
    r.type = channel.type;
    r.is_thread = channel.is_thread();
    return r;
}

namespace {

std::optional<std::string> forum_for(const std::optional<std::string>& guild_id, const ManagedForumIds& forums) {
    if (!guild_id || guild_id->empty()) return std::nullopt;
    auto it = forums.find(*guild_id);
    if (it == forums.end() || it->second.empty()) return std::nullopt;
    return it->second;
}

bool matches_thread(const ForumChannelRef* channel, const std::string& guild_id, const std::string& forum_id) {
    return channel && channel->id && !channel->id->empty() && channel->guild_id == guild_id && channel->parent_id == forum_id &&
           channel->is_thread;
}

} // namespace

bool is_managed_forum_thread(const ForumChannelRef* thread, const ManagedForumIds& forums) {
    if (!thread) return false;
    const auto forum_id = forum_for(thread->guild_id, forums);
    if (!forum_id || !matches_thread(thread, *thread->guild_id, *forum_id)) return false;
    const ForumChannelRef* parent = thread->parent.get();
    return parent && parent->id == *forum_id && parent->guild_id == *thread->guild_id && parent->type == channel_type::GuildForum;
}

bool is_managed_forum_location(const ForumLocation& location, const ManagedForumIds& forums, const FetchForumChannel& fetch_channel) {
    const auto forum_id = forum_for(location.guild_id, forums);
    if (!forum_id || !location.channel_id || location.channel_id->empty()) return false;
    const std::string& guild_id = *location.guild_id;
    try {
        std::optional<ForumChannelRef> channel = location.channel;
        if (!channel) {
            if (!fetch_channel) return false;
            channel = fetch_channel(*location.channel_id);
        }
        if (!channel || channel->id != *location.channel_id || !matches_thread(&*channel, guild_id, *forum_id)) return false;
        std::shared_ptr<const ForumChannelRef> parent = channel->parent;
        if (!parent) {
            if (!fetch_channel) return false;
            auto fetched = fetch_channel(*forum_id);
            if (!fetched) return false;
            parent = std::make_shared<const ForumChannelRef>(*fetched);
        }
        ForumChannelRef check;
        check.id = channel->id;
        check.guild_id = channel->guild_id;
        check.parent_id = channel->parent_id;
        check.parent = parent;
        check.is_thread = true;
        return is_managed_forum_thread(&check, forums);
    } catch (...) {
        // Failed resolution cannot authorize ingestion, acknowledgement or state changes.
        return false;
    }
}

FetchForumChannel rest_forum_channel_fetcher(DiscordApi& api) {
    return [&api](const std::string& channel_id) -> std::optional<ForumChannelRef> {
        return forum_ref_from_channel(api.get_channel(channel_id));
    };
}

} // namespace di::discord_hook
