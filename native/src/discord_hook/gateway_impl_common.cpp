// @spec Di管理フォーラムの受信境界
// DiscordGateway::Impl のメンバ関数: 共通・振り分け・門・READY。宣言と状態は gateway_impl.hpp。
#include "discord_hook/gateway_impl.hpp"

namespace di::discord_hook {

auto DiscordGateway::Impl::forums() const -> ManagedForumIds {
    std::lock_guard lock(mu);
    return managed_ids;
}

/** 読み取り fetch (同一性だけをキャッシュする。取得失敗は例外 = 境界は拒否)。 */
auto DiscordGateway::Impl::fetch_channel(const std::string& id) -> std::optional<ForumChannelRef> {
    const auto now = std::chrono::steady_clock::now();
    if (deps.channelCacheTtl.count() > 0) {
        std::lock_guard lock(mu);
        auto it = channel_cache.find(id);
        if (it != channel_cache.end() && now - it->second.first < deps.channelCacheTtl) return it->second.second;
    }
    ForumChannelRef ref = forum_ref_from_channel(api->get_channel(id));
    if (deps.channelCacheTtl.count() > 0) {
        std::lock_guard lock(mu);
        if (channel_cache.size() >= deps.channelCacheCapacity) channel_cache.clear();
        channel_cache[id] = {now, ref};
    }
    return ref;
}

auto DiscordGateway::Impl::fetcher() -> FetchForumChannel {
    return [this](const std::string& id) { return fetch_channel(id); };
}

auto DiscordGateway::Impl::managed_location(const std::optional<std::string>& guild_id, const std::string& channel_id, const std::optional<ForumChannelRef>& channel) -> bool {
    return is_managed_forum_location(ForumLocation{guild_id, channel_id, channel}, forums(), fetcher());
}

auto DiscordGateway::Impl::is_managed_scene(const std::optional<std::string>& scene) -> bool {
    const auto parsed = parse_discord_scene(scene);
    return parsed && managed_location(parsed->guildId, parsed->channelId);
}

auto DiscordGateway::Impl::submit_background(std::function<void()> task) -> bool {
    if (flow_live && flow_live->submitBackground) return flow_live->submitBackground(std::move(task));
    return background_executor().submit(std::move(task));
}

auto DiscordGateway::Impl::sleep_for(std::chrono::milliseconds ms) -> void {
    if (deps.sleep) deps.sleep(ms);
    else std::this_thread::sleep_for(ms);
}

auto DiscordGateway::Impl::enqueue(bool critical, const char* what, std::function<void()> task) -> void {
    Executor& ex = critical ? *priority : *normal;
    const bool ok = ex.submit(std::move(task));
    std::lock_guard lock(mu);
    if (ok) {
        (critical ? stats.priorityQueued : stats.normalQueued)++;
        return;
    }
    if (critical) {
        stats.priorityDropped++;
        log::error(std::string("  discord-gateway: 起動系キューが満杯のため ") + what + " を処理できない");
    } else {
        stats.normalDropped++;
        log::warn(std::string("  discord-gateway: キューが満杯のため ") + what + " を捨てた");
    }
}

auto DiscordGateway::Impl::handle_dispatch(const std::string& event, const Json& d) -> void {
    if (stopped || !d.is_object()) return;
    auto self = shared_from_this();
    if (event == "READY") {
        {
            std::lock_guard lock(mu);
            self_id = json_string(json_object(d, "user"), "id");
            if (auto app = json_opt_string(json_object(d, "application"), "id")) ready_app_id = app;
        }
        // discord.js の client.once(ClientReady): 再 identify の READY では繰り返さない。
        if (!ready_started.exchange(true)) enqueue(true, "READY", [self, d] { self->on_ready(d); });
        return;
    }
    if (event == "THREAD_CREATE") {
        enqueue(true, "THREAD_CREATE", [self, d] { self->on_thread_create(d); });
    } else if (event == "INTERACTION_CREATE") {
        enqueue(true, "INTERACTION_CREATE", [self, d] { self->on_interaction_create(d); });
    } else if (event == "MESSAGE_REACTION_ADD") {
        const bool approval = json_string(json_object(d, "emoji"), "name") == "✅";
        enqueue(approval, "MESSAGE_REACTION_ADD", [self, d] { self->on_reaction_add(d); });
    } else if (event == "MESSAGE_CREATE") {
        enqueue(false, "MESSAGE_CREATE", [self, d] { self->on_message_create(d); });
    }
}

auto DiscordGateway::Impl::count_rejected(bool passed) -> void {
    if (passed) return;
    std::lock_guard lock(mu);
    stats.gateRejected++;
}

auto DiscordGateway::Impl::on_thread_create(const Json& d) -> void {
    ForumThreadEvent ev;
    ev.thread.id = json_opt_string(d, "id");
    ev.thread.guild_id = json_opt_string(d, "guild_id");
    ev.thread.parent_id = json_opt_string(d, "parent_id");
    ev.thread.type = static_cast<int>(json_int(d, "type", -1));
    ev.thread.is_thread = is_thread_type(*ev.thread.type);
    ev.newly_created = json_bool(d, "newly_created", false);
    ev.raw = d;
    count_rejected(gate->on_thread_create(ev));
}

auto DiscordGateway::Impl::message_event(const Json& d) -> ForumMessageEvent {
    ForumMessageEvent m;
    m.guild_id = json_opt_string(d, "guild_id");
    m.channel_id = json_opt_string(d, "channel_id");
    const auto& author = json_object(d, "author");
    // webhook 投稿 (persona の発話) も bot として扱う (discord.js の author.bot は webhook で true)。
    m.author_bot = json_bool(author, "bot", false) || json_opt_string(d, "webhook_id").has_value();
    m.has_reference = d.contains("message_reference") && d["message_reference"].is_object();
    m.raw = d;
    return m;
}

auto DiscordGateway::Impl::on_message_create(const Json& d) -> void { count_rejected(gate->on_message_create(message_event(d))); }

auto DiscordGateway::Impl::on_reaction_add(const Json& d) -> void {
    ForumReactionEvent ev;
    ev.message.guild_id = json_opt_string(d, "guild_id");
    ev.message.channel_id = json_opt_string(d, "channel_id");
    const std::string user_id = json_string(d, "user_id");
    std::string me;
    {
        std::lock_guard lock(mu);
        me = self_id;
    }
    const auto& member_user = json_object(json_object(d, "member"), "user");
    // bot 自身のリアクション (👀 / ☑️ / 🏆 / 👍) と他の bot のリアクションは承認・スコアに使わない。
    ev.user_bot = json_bool(member_user, "bot", false) || (!me.empty() && user_id == me);
    ev.raw = d;
    count_rejected(gate->on_reaction_add(ev));
}

auto DiscordGateway::Impl::on_interaction_create(const Json& d) -> void {
    ForumInteractionEvent ev;
    ev.guild_id = json_opt_string(d, "guild_id");
    ev.channel_id = json_opt_string(d, "channel_id");
    const auto& data = json_object(d, "data");
    const auto type = json_int(d, "type", 0);
    if ((type == 3 || type == 5) && data.is_object()) ev.custom_id = json_opt_string(data, "custom_id");
    ev.raw = d;
    count_rejected(gate->on_interaction_create(ev));
}

auto DiscordGateway::Impl::on_ready(const Json& d) -> void {
    const auto& user = json_object(d, "user");
    log::info("  discord-gateway: logged in as " + json_string(user, "username"));
    // slash command を Discord に登録 (これが無いとクライアントに候補が出ない)。
    try {
        std::string app_id = deps.applicationId.value_or("");
        if (app_id.empty()) {
            std::lock_guard lock(mu);
            app_id = ready_app_id.value_or(self_id);
        }
        const auto r = register_slash_commands(*api, app_id, guild_ids);
        log::info("  discord-gateway: registered " + std::to_string(r.commandCount) + " slash commands (" + r.scope +
                  (r.scope == "guild" ? " x" + std::to_string(r.guildIds.size()) : std::string(" — 反映に最大1時間")) + ")");
    } catch (const std::exception& e) {
        log::warn("  discord-gateway: slash command 登録失敗: " + what_of(e));
    }

    // guild ごとに monitor 状態カードを設置 (複数サーバ対応)。
    if (!guild_ids.empty()) {
        if (deps.monitorCards) {
            for (const auto& guild_id : guild_ids) {
                auto card = std::make_unique<MonitorCard>(MonitorCardDeps{api, deps.router.workspaceId, guild_id});
                card->start();
                std::lock_guard lock(mu);
                monitors.push_back(std::move(card));
            }
        }
    } else {
        log::info("  discord-monitor: skipped (set discord.guildIds to enable)");
    }

    // フォーラム集約: データ学習依頼 / まとめ投稿 を自動作成し、前者を crawl 監視に足す。
    if (forum_enabled && deps.forum && !guild_ids.empty()) {
        try {
            ManagedChannelNames names{deps.forum->dataLearningChannelName, deps.forum->summaryChannelName, deps.forum->managedCategoryName};
            const auto r = ensure_managed_channels(*api, guild_ids, names);
            {
                std::lock_guard lock(mu);
                for (const auto& id : r.dataLearningChannelIds) crawl_ids.insert(id);
            }
            log::info("  discord-forum: managed output channels ensured");
        } catch (const std::exception& e) {
            log::warn("  discord-forum: managed channel 作成失敗: " + what_of(e));
        }
    }

    // 議論フォーラムを ensure し、定義済みフロータグを用意する。LLM未設定でも slash の受信境界に同じ対象IDを使う。
    if (forum_enabled && !guild_ids.empty()) {
        const std::string forum_name = deps.forum ? str::trim(deps.forum->discussionForumName) : std::string();
        for (const auto& guild_id : guild_ids) {
            if (forum_name.empty()) {
                log::warn("  discord-forum: managed forum name missing; guild=" + guild_id + " denied");
                continue;
            }
            std::optional<Channel> forum;
            try {
                forum = ensure_discussion_forum(*api, guild_id, forum_name);
            } catch (...) {
                forum.reset();
            }
            if (forum && forum->guild_id == guild_id && forum->type == channel_type::GuildForum) {
                std::lock_guard lock(mu);
                managed_ids[guild_id] = forum->id;
            } else {
                log::warn("  discord-forum: managed forum unresolved; guild=" + guild_id + " denied");
            }
        }
    }

    // ゲーム感想カテゴリを ensure (無ければ作成)。
    if (deps.gameFeedback && deps.gameFeedback->enabled && !guild_ids.empty()) {
        try {
            ensure_game_feedback_category(*api, guild_ids, deps.gameFeedback->categoryName);
        } catch (const std::exception& e) {
            log::warn("  game-feedback: ensure 失敗: " + what_of(e));
        }
    }
    ready_finished = true;
}

} // namespace di::discord_hook
