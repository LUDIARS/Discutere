#include "discord_hook/forum_event_gate.hpp"

#include "base/log.hpp"
#include "discord_hook/forum_flow_tags.hpp"
#include "discord_hook/paper_gap_ui.hpp"

namespace di::discord_hook {

bool component_matches_channel(const std::string& custom_id, const std::optional<std::string>& channel_id) {
    std::optional<std::string> target = parse_flow_custom_btn_custom_id(custom_id);
    if (!target) target = parse_flow_modal_custom_id(custom_id);
    if (!target) target = parse_flow_pick_custom_id(custom_id);
    if (!target) target = parse_flow_settings_custom_id(custom_id);
    if (!target) {
        if (auto gap = parse_paper_gap_modal_custom_id(custom_id)) target = gap->channelId;
    }
    return !target || (channel_id && *target == *channel_id);
}

ForumEventGate::ForumEventGate(ManagedForumIdsProvider forums, FetchForumChannel fetch_channel, ForumEventHandlers handlers)
    : forums_(std::move(forums)), fetch_channel_(std::move(fetch_channel)), handlers_(std::move(handlers)) {}

bool ForumEventGate::managed(const ForumLocation& location) const {
    try {
        const ManagedForumIds forums = forums_ ? forums_() : ManagedForumIds{};
        return is_managed_forum_location(location, forums, fetch_channel_);
    } catch (...) {
        return false;  // fail closed
    }
}

namespace {
template <class Fn>
bool dispatch(const char* what, Fn&& fn) {
    try {
        fn();
        return true;
    } catch (...) {
        log::warn(std::string("  discord-gateway: managed ") + what + " handler failed");
        return false;
    }
}
} // namespace

bool ForumEventGate::on_message_create(const ForumMessageEvent& message) const {
    if (message.author_bot || message.has_reference) return false;
    if (!managed(ForumLocation{message.guild_id, message.channel_id, message.channel})) return false;
    if (!handlers_.messageCreate) return false;
    return dispatch("message", [&] { handlers_.messageCreate(message); });
}

bool ForumEventGate::on_reaction_add(const ForumReactionEvent& reaction) const {
    if (reaction.user_bot) return false;
    const auto& m = reaction.message;
    if (!managed(ForumLocation{m.guild_id, m.channel_id, m.channel})) return false;
    if (!handlers_.reactionAdd) return false;
    return dispatch("reaction", [&] { handlers_.reactionAdd(reaction); });
}

bool ForumEventGate::on_interaction_create(const ForumInteractionEvent& interaction) const {
    if (interaction.custom_id && !component_matches_channel(*interaction.custom_id, interaction.channel_id)) return false;
    if (!managed(ForumLocation{interaction.guild_id, interaction.channel_id, interaction.channel})) return false;
    if (!handlers_.interactionCreate) return false;
    return dispatch("interaction", [&] { handlers_.interactionCreate(interaction); });
}

bool ForumEventGate::on_thread_create(const ForumThreadEvent& thread) const {
    if (!thread.newly_created || !handlers_.threadCreate) return false;
    if (!managed(ForumLocation{thread.thread.guild_id, thread.thread.id, thread.thread})) return false;
    return dispatch("thread", [&] { handlers_.threadCreate(thread, thread.newly_created); });
}

ForumEventGate bind_managed_forum_events(ManagedForumIdsProvider forums, FetchForumChannel fetch_channel, ForumEventHandlers handlers) {
    return ForumEventGate(std::move(forums), std::move(fetch_channel), std::move(handlers));
}

} // namespace di::discord_hook
