// DiscordGateway::Impl のメンバ関数: 外向き (まとめ・リアクション・掃除・停止)。宣言と状態は gateway_impl.hpp。
#include "discord_hook/gateway_impl.hpp"

namespace di::discord_hook {

auto DiscordGateway::Impl::finalize(const std::optional<std::string>& scene, const std::string& summary, const std::string& title) -> FinalizeForumPostResult {
    if (!is_managed_scene(scene)) return FinalizeForumPostResult{false, std::string("outside managed forum")};
    FinalizeForumPostArgs args;
    args.scene = scene;
    args.summary = summary;
    args.title = title;
    if (deps.forum) {
        args.summaryChannelName = deps.forum->summaryChannelName;
        args.categoryName = deps.forum->managedCategoryName;
    }
    return discord_hook::finalize_forum_post(*api, args);
}

auto DiscordGateway::Impl::react(const std::string& channel_id, const std::string& message_id, const std::string& emoji) -> void {
    try {
        if (forums().empty()) return;
        const auto ref = fetch_channel(channel_id);
        if (!ref || !managed_location(ref->guild_id, channel_id, ref)) return;
        api->create_reaction(channel_id, message_id, emoji);
    } catch (...) {
        // best-effort: メッセージ削除済 / 権限不足等は握り潰す
    }
}

auto DiscordGateway::Impl::sweep(std::optional<int> limit_opt) -> SweepUnseededResult {
    const int limit = std::clamp(limit_opt.value_or(30), 1, 100);
    SweepUnseededResult out;
    std::string bot_id;
    {
        std::lock_guard lock(mu);
        bot_id = self_id;
    }
    auto sweep_channel = [&](const std::string& channel_id, const std::optional<std::string>& parent_channel_id) {
        std::optional<ForumChannelRef> ref;
        try {
            ref = fetch_channel(channel_id);
        } catch (...) {
            return;
        }
        if (!ref || !managed_location(ref->guild_id, channel_id, ref)) return;
        Json messages;
        try {
            RequestOptions o;
            o.query.emplace_back("limit", std::to_string(limit));
            messages = api->request("GET", "/channels/" + channel_id + "/messages", std::nullopt, o);
        } catch (...) {
            return;
        }
        if (!messages.is_array()) return;
        for (const auto& raw : messages) {
            try {
                if (!raw.is_object()) continue;
                Message msg = message_from_json(raw);
                if (!msg.guild_id) msg.guild_id = ref->guild_id;
                if (!managed_location(msg.guild_id, msg.channel_id)) continue;
                if (msg.author.bot || msg.webhook_id) continue;
                // 過去投稿の再走査でも、ライブ受信と同様にリプライは取り込まない。
                if (msg.message_reference) continue;
                // 既にこのボットが 👀 を付けている = 検知済なのでスキップ。
                bool seen = false;
                for (const auto& r : json_array(raw, "reactions")) {
                    if (!r.is_object() || json_string(json_object(r, "emoji"), "name") != "👀") continue;
                    if (r.contains("me")) {
                        seen = json_bool(r, "me", false);
                    } else if (!bot_id.empty()) {
                        const Json users = api->request("GET", "/channels/" + channel_id + "/messages/" + msg.id + "/reactions/" +
                                                                  url_encode_emoji("👀"));
                        if (users.is_array())
                            for (const auto& u : users) seen = seen || json_string(u, "id") == bot_id;
                    }
                }
                if (seen) continue;
                out.scanned++;
                const auto normalized = normalize_discord_inbound_message(raw);
                if (!normalized) continue;
                const auto r = route_inbound_message(*normalized, msg.guild_id.value_or("dm"), deps.router, parent_channel_id);
                if (r.ingested && r.seed && r.seed()) {
                    out.seeded++;
                    try {
                        api->create_reaction(msg.channel_id, msg.id, "👀");
                    } catch (...) {
                    }
                }
            } catch (const std::exception& e) {
                log::warn("  discord-seed-sweep: message 失敗: " + what_of(e));
            }
        }
    };
    // フォーラム有効時は guild 内のアクティブスレッドも走査 (親=スレッドの親 id を渡す)。
    if (forum_enabled) {
        for (const auto& [guild_id, forum_id] : forums()) {
            (void)forum_id;
            try {
                const Json active = api->request("GET", "/guilds/" + guild_id + "/threads/active");
                for (const auto& t : json_array(active, "threads")) {
                    if (!t.is_object()) continue;
                    Channel thread = channel_from_json(t);
                    if (managed_location(thread.guild_id, thread.id, forum_ref_from_channel(thread))) sweep_channel(thread.id, thread.parent_id);
                }
            } catch (const std::exception& e) {
                log::warn("  discord-seed-sweep: guild " + guild_id + " thread 走査失敗: " + what_of(e));
            }
        }
    }
    return out;
}

auto DiscordGateway::Impl::url_encode_emoji(const std::string& emoji) -> std::string {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : emoji) {
        out += '%';
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

auto DiscordGateway::Impl::stop() -> void {
    if (stopped.exchange(true)) return;
    if (client) client->stop();
    std::vector<std::unique_ptr<MonitorCard>> ms;
    {
        std::lock_guard lock(mu);
        managed_ids.clear();
        ms.swap(monitors);
    }
    for (auto& m : ms) m->stop();
    if (priority) priority->stop();
    if (normal) normal->stop();
    if (api) api->shutdown();
}

} // namespace di::discord_hook
