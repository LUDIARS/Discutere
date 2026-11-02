#include "discord_hook/normalize.hpp"

#include "base/strings.hpp"

namespace di::discord_hook {

namespace {

/** 文字列なら trim、空なら nullopt。 */
std::optional<std::string> as_string(const Json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return std::nullopt;
    auto s = str::trim(it->get<std::string>());
    if (s.empty()) return std::nullopt;
    return s;
}

// JS の typeof v === "object" && v !== null (配列も object)。配列はキーを持たないので空として扱う。
bool is_record(const Json& v) { return v.is_object() || v.is_array(); }

std::optional<DiscordInboundAuthor> as_author(const Json& v) {
    if (!v.is_object()) return std::nullopt;
    auto id = as_string(v, "id");
    auto username = as_string(v, "username");
    if (!id || !username) return std::nullopt;
    DiscordInboundAuthor a{*id, *username, std::nullopt};
    if (v.contains("bot") && v["bot"].is_boolean()) a.bot = v["bot"].get<bool>();
    return a;
}

std::optional<DiscordInboundMessage> from_raw_message(const Json& raw) {
    if (!raw.is_object()) return std::nullopt;
    auto id = as_string(raw, "id");
    auto channel_id = as_string(raw, "channel_id");
    auto content = as_string(raw, "content");
    auto timestamp = as_string(raw, "timestamp");
    std::optional<DiscordInboundAuthor> author;
    if (raw.contains("author")) author = as_author(raw["author"]);
    if (!id || !channel_id || !content || !timestamp || !author) return std::nullopt;
    DiscordInboundMessage m;
    m.id = *id;
    m.channelId = *channel_id;
    m.author = *author;
    m.content = *content;
    m.timestamp = *timestamp;
    if (raw.contains("mentions") && raw["mentions"].is_array()) {
        for (const auto& item : raw["mentions"]) {
            if (item.is_array()) {  // JS では配列も object (キーは無い)
                m.mentions.push_back(DiscordInboundMention{});
                continue;
            }
            if (!item.is_object()) continue;
            m.mentions.push_back(DiscordInboundMention{as_string(item, "id"), as_string(item, "username")});
        }
    }
    return m;
}

std::optional<DiscordInboundMessage> normalize(const Json& payload, int depth) {
    if (!is_record(payload) || depth > 16 || !payload.is_object()) return std::nullopt;
    // Raw webhook style: { id, channel_id, author, content, mentions, timestamp }
    if (payload.contains("content") && payload["content"].is_string() && payload.contains("author") && is_record(payload["author"]))
        return from_raw_message(payload);
    // Gateway style: { t: "MESSAGE_CREATE", d: {...message...} }
    if (payload.contains("t") && payload["t"] == "MESSAGE_CREATE" && payload.contains("d") && is_record(payload["d"]))
        return from_raw_message(payload["d"]);
    // Event wrapper style: { event: { ...message... } }
    if (payload.contains("event") && is_record(payload["event"])) return normalize(payload["event"], depth + 1);
    return std::nullopt;
}

} // namespace

std::optional<DiscordInboundMessage> normalize_discord_inbound_message(const Json& payload) { return normalize(payload, 0); }

} // namespace di::discord_hook
