#include "discord_hook/discussion_bridge.hpp"

#include <map>
#include <mutex>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "db/repository.hpp"
#include "discord_hook/poster.hpp"

namespace di::discord_hook {

namespace {

struct ApiCache {
    std::mutex m;
    DiscordApiFactory factory;
    // token そのものではなく sha256 を鍵にする (鍵の一覧から token を辿れないように)。
    std::map<std::string, std::shared_ptr<DiscordApi>> by_token_hash;
};

ApiCache& api_cache() {
    static ApiCache c;
    return c;
}

std::shared_ptr<DiscordApi> api_for(const std::string& token) {
    auto& c = api_cache();
    std::lock_guard lock(c.m);
    if (c.factory) return c.factory(token);
    const auto key = crypto::sha256_hex(token);
    auto& slot = c.by_token_hash[key];
    if (!slot) {
        DiscordApiOptions o;
        o.token = token;
        slot = std::make_shared<DiscordApi>(o);
    }
    return slot;
}

DiscussionPostResult fail(std::string reason) {
    DiscussionPostResult r;
    r.reason = std::move(reason);
    return r;
}

DiscussionPostResult posted(const std::string& channel_id, const std::string& message_id) {
    DiscussionPostResult r;
    r.ok = true;
    r.channelId = channel_id;
    r.messageId = message_id;
    return r;
}

} // namespace

Json to_json(const DiscussionPostResult& r) {
    Json j = Json::object();
    j["ok"] = r.ok;
    if (r.skipped) j["skipped"] = true;
    if (r.reason) j["reason"] = *r.reason;
    if (r.channelId) j["channelId"] = *r.channelId;
    if (r.messageId) j["messageId"] = *r.messageId;
    return j;
}

void set_discussion_bridge_api_factory(DiscordApiFactory factory) {
    auto& c = api_cache();
    std::lock_guard lock(c.m);
    c.factory = std::move(factory);
    c.by_token_hash.clear();
}

DiscussionPostResult post_discussion_to_discord(const DiscussionPostArgs& args) {
    if (!args.core) return fail("core not provided");
    // session.scene = "discord:<guildId>/<channelId>" or "gap:<gapId>" (gap session 用)
    const auto session = args.core->raw().get("SELECT scene FROM sessions WHERE id = ?", {args.sessionId});
    const auto scene = session ? session->opt_str("scene") : std::nullopt;
    if (!scene || scene->empty() || !str::starts_with(*scene, "discord:")) {
        // headless (gap:<gapId> 等) の議論は Discord に出さないのが正常動作 (skipped で返し caller の warn を抑止)。
        DiscussionPostResult r;
        r.skipped = true;
        r.reason = "session.scene is not discord-bound (headless)";
        return r;
    }
    const auto parts = str::split(scene->substr(std::string_view("discord:").size()), "/");
    const std::string guild_id = parts.empty() ? std::string() : parts[0];
    const std::string channel_id = parts.size() > 1 ? parts[1] : std::string();
    if (channel_id.empty()) return fail("session.scene channel parse failed");

    std::optional<std::string> bot_token;
    for (const auto& m : db::MonitorRepo().find_by_workspace_id(args.workspaceId)) {
        if (m.platform == "discord" && m.botToken && !m.botToken->empty() && (guild_id.empty() || m.botWorkspaceId == guild_id)) {
            bot_token = m.botToken;
            break;
        }
    }
    // Discord-only pivot: channel_monitors に登録が無くても、Gateway 接続と同じ config の bot token で投稿する。
    if (!bot_token) bot_token = config::get_config().discord.botToken;
    if (!bot_token || bot_token->empty()) return fail("no discord bot token (channel_monitors / config どちらも未設定)");

    auto api = api_for(*bot_token);
    const auto body = humanize_for_discord(args.text);

    // 全 persona (進行役含む) を webhook で人間名として投稿する (役割名は出さない)。
    if (args.viaWebhook) {
        try {
            // フォーラムスレッドは webhook を持てないので、親フォーラムの webhook + thread_id でスレッドに流す。
            const auto target = resolve_webhook_target(*api, channel_id);
            const auto wh = ensure_channel_webhook(*api, target.webhookChannelId);
            DiscordWebhookPostArgs w;
            w.webhookId = wh.id;
            w.webhookToken = wh.token;
            w.username = args.speakerLabel;
            w.content = body;
            w.threadId = target.threadId;
            return posted(channel_id, post_discord_webhook(*api, w));
        } catch (const std::exception& err) {
            // webhook 権限が無い等で失敗 → bot 直接投稿に fallback (名前を本文先頭に)
            try {
                return posted(channel_id, post_discord_channel(*api, {channel_id, "**" + args.speakerLabel + "**\n" + body, std::nullopt}));
            } catch (const std::exception& err2) {
                return fail(std::string("webhook(") + err.what() + ") / bot(" + err2.what() + ")");
            }
        }
    }

    // viaWebhook=false の経路 (名前なし bot 投稿)。
    try {
        return posted(channel_id, post_discord_channel(*api, {channel_id, body, std::nullopt}));
    } catch (const std::exception& err) {
        return fail(err.what());
    }
}

} // namespace di::discord_hook
