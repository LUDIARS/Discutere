// @spec Di管理フォーラムの受信境界
#include "discord_hook/gateway.hpp"
#include "discord_hook/gateway_impl.hpp"

#include <algorithm>
#include <cmath>

namespace di::discord_hook {

// ─── 純粋な補助 ─────────────────────────────────────────────────

namespace {

bool ends_with_ci(const std::string& s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    const std::string tail = str::to_lower(std::string_view(s).substr(s.size() - suffix.size()));
    return tail == suffix;
}

} // namespace

bool is_text_like_attachment(const std::string& name, const std::optional<std::string>& content_type) {
    // contentType.startsWith("text/") は大文字小文字を区別する。/^(application\/(json|xml|x-yaml)|text\/markdown)/ も同じ。
    if (content_type && !content_type->empty()) {
        const std::string& ct = *content_type;
        if (str::starts_with(ct, "text/")) return true;
        for (const char* p : {"application/json", "application/xml", "application/x-yaml", "text/markdown"})
            if (str::starts_with(ct, p)) return true;
    }
    // /\.(md|markdown|txt|text|json|ya?ml|csv|rst|adoc)$/i
    for (const char* ext : {".md", ".markdown", ".txt", ".text", ".json", ".yaml", ".yml", ".csv", ".rst", ".adoc"})
        if (ends_with_ci(name, ext)) return true;
    return false;
}

std::map<std::string, std::string> modal_field_values(const Json& data) {
    std::map<std::string, std::string> out;
    // 入れ子の深さ・要素数は Gateway の JSON 上限 (gateway_client の json_limits) で抑えられている。さらに深さを 8 に切る。
    std::function<void(const Json&, int)> walk = [&](const Json& node, int depth) {
        if (depth > 8) return;
        if (node.is_array()) {
            for (const auto& c : node) walk(c, depth + 1);
            return;
        }
        if (!node.is_object()) return;
        if (node.contains("custom_id") && node["custom_id"].is_string() && node.contains("value") && node["value"].is_string())
            out.emplace(node["custom_id"].get<std::string>(), node["value"].get<std::string>());
        if (node.contains("components")) walk(node["components"], depth + 1);
        if (node.contains("component")) walk(node["component"], depth + 1);
    };
    if (data.is_object() && data.contains("components")) walk(data["components"], 0);
    return out;
}

std::optional<double> positive_number_field(const std::string& raw) {
    const auto n = str::js_number(str::trim(raw));
    if (!n || !std::isfinite(*n) || !(*n > 0)) return std::nullopt;
    return n;
}

// ─── DiscordGateway ─────────────────────────────────────────────

DiscordGateway::DiscordGateway(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
DiscordGateway::~DiscordGateway() { stop(); }
void DiscordGateway::stop() { impl_->stop(); }
FinalizeForumPostResult DiscordGateway::finalize_forum_post(const std::optional<std::string>& scene, const std::string& summary,
                                                            const std::string& title) {
    return impl_->finalize(scene, summary, title);
}
void DiscordGateway::react_to_message(const std::string& channel_id, const std::string& message_id, const std::string& emoji) {
    impl_->react(channel_id, message_id, emoji);
}
SweepUnseededResult DiscordGateway::sweep_unseeded(std::optional<int> limit) { return impl_->sweep(limit); }
StartExistingThreadResult DiscordGateway::start_existing_thread(const std::string& thread_id) { return impl_->start_existing_thread(thread_id); }
void DiscordGateway::handle_dispatch(const std::string& event, const Json& d) { impl_->handle_dispatch(event, d); }
bool DiscordGateway::ready_done() const { return impl_->ready_finished.load(); }
bool DiscordGateway::wait_idle(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (impl_->priority->queued() == 0 && impl_->priority->running() == 0 && impl_->normal->queued() == 0 &&
            impl_->normal->running() == 0)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
ManagedForumIds DiscordGateway::managed_forum_ids() const { return impl_->forums(); }
std::vector<std::string> DiscordGateway::crawl_channel_ids() const {
    std::lock_guard lock(impl_->mu);
    return {impl_->crawl_ids.begin(), impl_->crawl_ids.end()};
}
GatewayEventStats DiscordGateway::event_stats() const {
    std::lock_guard lock(impl_->mu);
    return impl_->stats;
}
GatewayStats DiscordGateway::gateway_stats() const { return impl_->client ? impl_->client->stats() : GatewayStats{}; }
std::shared_ptr<DiscordApi> DiscordGateway::api() const { return impl_->api; }

std::unique_ptr<DiscordGateway> start_discord_gateway(DiscordGatewayDeps deps) {
    if (deps.botToken.empty()) {
        log::info("  discord-gateway: skipped (set discord.botToken to enable)");
        return nullptr;
    }
    auto impl = std::make_shared<DiscordGateway::Impl>(std::move(deps));
    auto& d = impl->deps;
    DiscordApiOptions rest = d.rest;
    rest.token = d.botToken;
    impl->api = std::make_shared<DiscordApi>(rest);
    impl->priority = std::make_unique<Executor>("discord-start", std::max<std::size_t>(1, d.priorityThreads), d.priorityQueue);
    impl->normal = std::make_unique<Executor>("discord-events", std::max<std::size_t>(1, d.normalThreads), d.normalQueue);
    impl->pending_picks = BoundedPending<PendingFlowPick>(d.maxPendingStarts);
    impl->pending_settings = BoundedPending<PendingFlowSettings>(d.maxPendingStarts);
    for (const auto& g : d.guildIds)
        if (!g.empty() && g != "dm") impl->guild_ids.push_back(g);
    for (const auto& c : d.crawlChannelIds)
        if (!c.empty()) impl->crawl_ids.insert(c);
    impl->forum_enabled = d.forum && d.forum->enabled;
    if (d.flowLive) {
        impl->flow_live = *d.flowLive;
        if (!impl->flow_live->api) impl->flow_live->api = impl->api;
    }
    // 新フローエンジンでフォーラム議論を回すか (flowLive が無ければ forum 起動 skip)。
    impl->flow_live_enabled = impl->forum_enabled && impl->flow_live.has_value();

    // /debate のパーティ議論ランナー (config.discussion があれば有効)。
    if (d.debate) {
        discussion::DebateRunnerOptions o;
        o.api = impl->api;
        o.discussion = *d.debate;
        o.gitBashPath = d.gitBashPath;
        impl->debate_runner = discussion::create_debate_runner(std::move(o));
    }

    // 収束時にフォーラムスレッドを締める (lock+archive + まとめ転記)。フローは Gateway より長く生きうるので弱参照。
    std::weak_ptr<DiscordGateway::Impl> weak = impl;
    impl->flow_hooks.onConcluded = [weak](const std::string& scene, const std::string& title, const std::string& summary) {
        auto self = weak.lock();
        if (!self || self->stopped) return;
        try {
            self->finalize(scene, summary, title);
        } catch (const std::exception& e) {
            log::warn("  flow-live: finalize 失敗: " + what_of(e));
        }
    };
    // 議論適性ゲート (09) のフロー再提案: 既存の議論タイプ選択メニューを再提示する。
    impl->flow_hooks.onReproposeFlowType = [weak](const std::string& guild_id, const std::string& thread_id, const std::string& theme,
                                                  const std::vector<flow::FlowTag>& tags) {
        auto self = weak.lock();
        if (!self || self->stopped) return;
        if (!self->is_managed_scene("discord:" + guild_id + "/" + thread_id)) return;
        {
            std::lock_guard lock(self->mu);
            PendingFlowPick p;
            p.guildId = guild_id;
            p.theme = theme;
            p.tags = tags;
            self->pending_picks.set(thread_id, p);
        }
        self->post_flow_pick_menu(thread_id);
    };

    ForumEventHandlers handlers;
    DiscordGateway::Impl* raw = impl.get();
    handlers.messageCreate = [raw](const ForumMessageEvent& m) { raw->handle_message(m); };
    handlers.reactionAdd = [raw](const ForumReactionEvent& r) { raw->handle_reaction(r); };
    handlers.interactionCreate = [raw](const ForumInteractionEvent& i) { raw->handle_interaction(i); };
    if (impl->flow_live_enabled)
        handlers.threadCreate = [raw](const ForumThreadEvent& t, bool) { raw->on_forum_thread_create(t.raw); };
    impl->gate.emplace(bind_managed_forum_events([raw] { return raw->forums(); }, raw->fetcher(), std::move(handlers)));

    if (d.connect) {
        GatewayClientOptions go = d.gatewayOptions;
        go.token = d.botToken;
        if (go.rest.base_url == DISCORD_API_BASE && rest.base_url != DISCORD_API_BASE) go.rest = rest;
        go.dispatch_in_background = false;  // 振り分けは handle_dispatch が自前のキューで行う
        go.on_dispatch = [weak](const std::string& event, const Json& payload) {
            if (auto self = weak.lock()) self->handle_dispatch(event, payload);
        };
        auto user_fatal = go.on_fatal;
        go.on_fatal = [user_fatal](int code, const std::string& reason) {
            log::warn("  discord-gateway error: close " + std::to_string(code) + " " + reason);
            if (user_fatal) user_fatal(code, reason);
        };
        impl->client = std::make_unique<GatewayClient>(std::move(go));
        impl->client->start();
    }
    return std::make_unique<DiscordGateway>(impl);
}

} // namespace di::discord_hook
