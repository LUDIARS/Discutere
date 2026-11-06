#include "discord_hook/paper_review_ack.hpp"

#include "base/log.hpp"
#include "base/strings.hpp"

namespace di::discord_hook {

AckableMessage ackable_message(DiscordApi& api, const Message& msg) {
    AckableMessage a;
    a.content = msg.content;
    a.has_reference = msg.message_reference.has_value();
    a.react = [&api, channel = msg.channel_id, id = msg.id](const std::string& emoji) { api.create_reaction(channel, id, emoji); };
    return a;
}

bool ack_paper_review_reply(const AckableMessage& msg, bool in_review, const std::function<void(const std::string&)>& warn) {
    if (!in_review || msg.has_reference || str::trim(msg.content).empty()) return false;
    try {
        if (msg.react) msg.react(PAPER_REVIEW_ACK_EMOJI);
    } catch (const std::exception& e) {
        const std::string m = std::string("  discord-forum: ペーパー返信への確認リアクション失敗: ") + e.what();
        if (warn) warn(m);
        else log::warn(m);
    }
    return true;
}

} // namespace di::discord_hook
