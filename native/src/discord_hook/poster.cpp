#include "discord_hook/poster.hpp"

#include <map>
#include <mutex>

#include "base/strings.hpp"
#include "base/url.hpp"

namespace di::discord_hook {

namespace {

constexpr const char* WEBHOOK_NAME = "Discutere議論";

std::string url_component(const std::string& s) { return url::encode_component(s); }

std::string truncate(const std::string& s, std::size_t max) {
    if (str::js_length(s) <= max) return s;
    return str::js_slice(s, 0, static_cast<std::int64_t>(max) - 1) + "…";
}

std::mutex& cache_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, WebhookCredentials>& webhook_cache() {
    static std::map<std::string, WebhookCredentials> c;
    return c;
}

} // namespace

std::string post_discord_channel(DiscordApi& api, const DiscordPostArgs& args) {
    Json body{{"content", truncate(args.content, 1900)}};
    if (args.replyTo && !args.replyTo->empty()) {
        body["message_reference"] = Json{{"message_id", *args.replyTo}};
        body["allowed_mentions"] = Json{{"replied_user", false}};
    }
    const Json res = api.request("POST", "/channels/" + url_component(args.channelId) + "/messages", body);
    return res.is_object() ? json_string(res, "id") : std::string();
}

void react_discord(DiscordApi& api, const std::string& channel_id, const std::string& message_id, const std::string& emoji) {
    api.create_reaction(channel_id, message_id, emoji);
}

WebhookCredentials ensure_channel_webhook(DiscordApi& api, const std::string& channel_id) {
    {
        std::lock_guard lock(cache_mutex());
        auto it = webhook_cache().find(channel_id);
        if (it != webhook_cache().end()) return it->second;
    }
    std::optional<WebhookCredentials> found;
    try {
        for (const auto& h : api.get_channel_webhooks(channel_id)) {
            if (h.token && !h.token->empty() && h.name.value_or("") == WEBHOOK_NAME) {
                found = WebhookCredentials{h.id, *h.token};
                break;
            }
        }
    } catch (const DiscordApiError&) {
        // 一覧が取れなくても作成を試みる (TS と同じ)
    }
    if (!found) {
        const auto created = api.create_webhook(channel_id, WEBHOOK_NAME);
        if (!created.token || created.token->empty()) throw DiscordApiError("create webhook: token missing", 200, 0, "POST /channels/:id/webhooks", false);
        found = WebhookCredentials{created.id, *created.token};
    }
    std::lock_guard lock(cache_mutex());
    webhook_cache()[channel_id] = *found;
    return *found;
}

void clear_webhook_cache() {
    std::lock_guard lock(cache_mutex());
    webhook_cache().clear();
}

WebhookTarget resolve_webhook_target(DiscordApi& api, const std::string& channel_id) {
    try {
        const auto ch = api.get_channel(channel_id);
        // 10=announcement thread, 11=public thread, 12=private thread
        if (ch.is_thread() && ch.parent_id && !ch.parent_id->empty()) return WebhookTarget{*ch.parent_id, channel_id};
        return WebhookTarget{channel_id, std::nullopt};
    } catch (const std::exception&) {
        return WebhookTarget{channel_id, std::nullopt};
    }
}

std::string post_discord_webhook(DiscordApi& api, const DiscordWebhookPostArgs& args) {
    Json body{{"content", truncate(args.content, 1900)}, {"username", str::js_slice(args.username, 0, 80)}};
    if (args.avatarUrl) body["avatar_url"] = *args.avatarUrl;
    RequestOptions o;
    o.auth = false;
    o.query.emplace_back("wait", "true");
    if (args.threadId) o.query.emplace_back("thread_id", *args.threadId);
    Json res;
    try {
        res = api.request("POST", "/webhooks/" + url_component(args.webhookId) + "/" + url_component(args.webhookToken), body, o);
    } catch (const DiscordApiError& e) {
        // 2xx だが本文が読めない応答は id 無しで成功扱い (TS の res.json().catch(() => ({})))。
        if (e.status() >= 200 && e.status() < 300) return "";
        throw;
    }
    return res.is_object() ? json_string(res, "id") : std::string();
}

std::string humanize_for_discord(const std::string& text) {
    std::string t = str::trim(text);
    // ^(反証|反例|弱点|指摘|反論|懸念)\s*[:：]\s*
    for (const char* label : {"反証", "反例", "弱点", "指摘", "反論", "懸念"}) {
        if (!str::starts_with(t, label)) continue;
        std::size_t i = std::string_view(label).size();
        auto skip_space = [&] {
            while (i < t.size()) {
                std::size_t j = i;
                if (!str::is_js_space(str::next_codepoint(t, j))) break;
                i = j;
            }
        };
        skip_space();
        if (t.compare(i, 1, ":") == 0) i += 1;
        else if (t.compare(i, 3, "：") == 0) i += 3;
        else break;
        skip_space();
        t = t.substr(i);
        break;
    }
    // ([。！？!?])(?!\n|$) → "$1\n"
    std::string out;
    out.reserve(t.size() + 16);
    std::size_t i = 0;
    while (i < t.size()) {
        const std::size_t start = i;
        const auto cp = str::next_codepoint(t, i);
        out.append(t, start, i - start);
        const bool punct = cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F || cp == '!' || cp == '?';
        if (punct && i < t.size() && t[i] != '\n') out.push_back('\n');
    }
    return out;
}

} // namespace di::discord_hook
