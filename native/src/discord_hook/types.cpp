#include "discord_hook/types.hpp"

namespace di::discord_hook {

Json to_json(const DiscordInboundAuthor& v) {
    Json o{{"id", v.id}, {"username", v.username}};
    if (v.bot) o["bot"] = *v.bot;
    return o;
}

Json to_json(const DiscordInboundMention& v) {
    Json o = Json::object();
    if (v.id) o["id"] = *v.id;
    if (v.username) o["username"] = *v.username;
    return o;
}

Json to_json(const DiscordInboundMessage& v) {
    Json o{{"id", v.id}, {"channelId", v.channelId}, {"author", to_json(v.author)}, {"content", v.content}};
    o["mentions"] = Json::array();
    for (const auto& m : v.mentions) o["mentions"].push_back(to_json(m));
    o["timestamp"] = v.timestamp;
    return o;
}

} // namespace di::discord_hook
