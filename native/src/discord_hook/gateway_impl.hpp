#pragma once
// gateway.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各単位が共有する、DiscordGateway::Impl の定義と
// その補助 (メンバ関数の本体は gateway_impl_*.cpp)。discord_hook/gateway_*.cpp 以外から include しない。
#include "discord_hook/gateway.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include "base/executor.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "discord_hook/crawl_handler.hpp"
#include "discord_hook/directive_handler.hpp"
#include "discord_hook/facilitator_directives.hpp"
#include "discord_hook/forum_event_gate.hpp"
#include "discord_hook/forum_flow_tags.hpp"
#include "discord_hook/game_feedback_channel.hpp"
#include "discord_hook/level_check.hpp"
#include "discord_hook/managed_channels.hpp"
#include "discord_hook/mechanics_check.hpp"
#include "discord_hook/monitor_card.hpp"
#include "discord_hook/normalize.hpp"
#include "discord_hook/paper_gap_ui.hpp"
#include "discord_hook/paper_review_ack.hpp"
#include "discord_hook/poster.hpp"
#include "discord_hook/register_commands.hpp"
#include "discord_hook/slash_interaction.hpp"
#include "discussion/director_live.hpp"
#include "flow/dispatch.hpp"

namespace di::discord_hook {

namespace gateway_detail {

constexpr const char* kFlowPickPrompt = "🏷️ 議論タイプのタグが付いていません。どのフローで進めますか？ (議論/改善/学習/壁打ち)";
constexpr const char* kFlowSettingsPrompt =
    "⚙️ 進行量を選んで議論を開始してください (ラウンド数 × ターン数)。「数値を指定」で任意の値も設定できます。";
constexpr const char* kSettingsExpired = "この設定は期限切れです。スレッドを作り直してください。";
constexpr const char* kPickExpired = "この選択は期限切れです。スレッドを作り直してください。";
constexpr const char* kNoPaperForGap =
    "このチャンネルには調整待ちのディスカッションペーパーがありません。レビュー中のフォーラムスレッドで実行してください。";
constexpr int kInteractionUpdateMessage = 7;
constexpr int kInteractionModal = 9;

inline std::string what_of(const std::exception& e) { return e.what(); }

/** 議論タイプ未指定の投稿に出した select の待ち。 */
struct PendingFlowPick {
    std::string guildId;
    std::string theme;
    std::vector<flow::FlowTag> tags;
    std::optional<double> rounds;
    std::optional<double> turnsPerRound;
    std::optional<std::vector<std::string>> opponentPersonaIds;
    std::optional<std::string> specText;
    std::optional<std::vector<std::string>> specAttachmentUrls;
};
/** 進行量設定 select/modal の待ち (フロー解決済み)。 */
struct PendingFlowSettings {
    std::string guildId;
    std::string theme;
    flow::FlowKind flow;
    std::vector<flow::FlowTag> tags;
    std::optional<std::vector<std::string>> opponentPersonaIds;
    std::optional<std::string> specText;
    std::optional<std::vector<std::string>> specAttachmentUrls;
};

/** 上限つきの待ち表 (挿入順に古いものから捨てる)。外部入力 (スレッド作成) で増えるため上限を課す。 */
template <class V>
class BoundedPending {
public:
    explicit BoundedPending(std::size_t cap) : cap_(cap == 0 ? 1 : cap) {}
    void set(const std::string& k, V v) {
        if (!map_.count(k)) order_.push_back(k);
        map_[k] = std::move(v);
        while (map_.size() > cap_ && !order_.empty()) {
            map_.erase(order_.front());
            order_.pop_front();
        }
    }
    std::optional<V> get(const std::string& k) const {
        auto it = map_.find(k);
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }
    bool has(const std::string& k) const { return map_.count(k) > 0; }
    std::optional<V> take(const std::string& k) {
        auto it = map_.find(k);
        if (it == map_.end()) return std::nullopt;
        V v = std::move(it->second);
        map_.erase(it);
        for (auto o = order_.begin(); o != order_.end(); ++o)
            if (*o == k) {
                order_.erase(o);
                break;
            }
        return v;
    }

private:
    std::size_t cap_;
    std::map<std::string, V> map_;
    std::deque<std::string> order_;
};

struct BeginFlowInfo {
    std::string guildId;
    std::string threadId;
    std::string theme;
    flow::FlowKind flow;
    std::vector<flow::FlowTag> tags;
    std::optional<double> rounds;
    std::optional<double> turnsPerRound;
    std::optional<std::vector<std::string>> opponentPersonaIds;
    std::optional<std::string> specText;
    std::optional<std::vector<std::string>> specAttachmentUrls;
};

/** interaction への応答口 (discord.js の reply / update / showModal / deferReply / editReply)。token はログに出さない。 */
class Ix {
public:
    Ix(std::shared_ptr<DiscordApi> api, const Json& d)
        : api_(std::move(api)), id_(json_string(d, "id")), token_(json_string(d, "token")), app_(json_string(d, "application_id")) {}
    bool valid() const { return !id_.empty() && !token_.empty(); }
    void reply(const std::string& content, bool ephemeral) {
        Json data{{"content", content}};
        if (ephemeral) data["flags"] = kEphemeralFlag;
        data["allowed_mentions"] = allowed_mentions_none();
        respond(Json{{"type", interaction_response::ChannelMessageWithSource}, {"data", data}});
    }
    void update(const std::string& content) {
        respond(Json{{"type", kInteractionUpdateMessage}, {"data", Json{{"content", content}, {"components", Json::array()}}}});
    }
    void show_modal(const Json& modal) { respond(Json{{"type", kInteractionModal}, {"data", modal}}); }
    void defer_ephemeral() {
        respond(Json{{"type", interaction_response::DeferredChannelMessageWithSource}, {"data", Json{{"flags", kEphemeralFlag}}}});
    }
    void edit_reply(const std::string& content) {
        api_->edit_original_interaction_response(app_, token_, Json{{"content", content}, {"allowed_mentions", allowed_mentions_none()}});
    }
    /** 失敗は握り潰す (TS の .catch(() => {}))。 */
    template <class F>
    void quietly(F&& f) {
        try {
            f();
        } catch (...) {
        }
    }
    bool replied() const { return replied_; }

private:
    void respond(const Json& body) {
        api_->create_interaction_response(id_, token_, body);
        replied_ = true;
    }
    std::shared_ptr<DiscordApi> api_;
    std::string id_;
    std::string token_;  // 秘密
    std::string app_;
    bool replied_ = false;
};

/** 先に defer 済みの診断返信口 (handle_*_check_command の defer は二重に送らない)。 */
class PreDeferredResponder : public InteractionResponder {
public:
    explicit PreDeferredResponder(RestInteractionResponder inner) : inner_(std::move(inner)) {}
    void defer_ephemeral() override {}
    void edit_reply(const DiagnosticReply& reply) override { inner_.edit_reply(reply); }

private:
    RestInteractionResponder inner_;
};

} // namespace gateway_detail

using namespace gateway_detail;

struct DiscordGateway::Impl : std::enable_shared_from_this<DiscordGateway::Impl> {
    DiscordGatewayDeps deps;
    std::shared_ptr<DiscordApi> api;
    std::unique_ptr<GatewayClient> client;
    std::unique_ptr<Executor> priority;
    std::unique_ptr<Executor> normal;
    std::unique_ptr<discussion::DebateRunner> debate_runner;
    std::optional<flow::FlowDiscordDeps> flow_live;
    flow::FlowLiveHooks flow_hooks;
    std::optional<ForumEventGate> gate;
    bool forum_enabled = false;
    bool flow_live_enabled = false;
    std::vector<std::string> guild_ids;

    mutable std::mutex mu;
    ManagedForumIds managed_ids;
    std::set<std::string> crawl_ids;
    BoundedPending<PendingFlowPick> pending_picks{1000};
    BoundedPending<PendingFlowSettings> pending_settings{1000};
    std::string self_id;
    std::optional<std::string> ready_app_id;
    std::vector<std::unique_ptr<MonitorCard>> monitors;
    GatewayEventStats stats;
    std::map<std::string, std::pair<std::chrono::steady_clock::time_point, ForumChannelRef>> channel_cache;
    std::atomic<bool> ready_started{false};
    std::atomic<bool> ready_finished{false};
    std::atomic<bool> stopped{false};

    explicit Impl(DiscordGatewayDeps d) : deps(std::move(d)) {}
    // ── 共通 ──
    ManagedForumIds forums() const;
    std::optional<ForumChannelRef> fetch_channel(const std::string& id);
    FetchForumChannel fetcher();
    bool managed_location(const std::optional<std::string>& guild_id, const std::string& channel_id, const std::optional<ForumChannelRef>& channel = std::nullopt);
    bool is_managed_scene(const std::optional<std::string>& scene);
    bool submit_background(std::function<void()> task);
    void sleep_for(std::chrono::milliseconds ms);
    // ── 振り分け (制御スレッド。重い仕事をしない) ──
    void enqueue(bool critical, const char* what, std::function<void()> task);
    void handle_dispatch(const std::string& event, const Json& d);
    void count_rejected(bool passed);
    // ── 門 (forum_event_gate) を通す ──
    void on_thread_create(const Json& d);
    static ForumMessageEvent message_event(const Json& d);
    void on_message_create(const Json& d);
    void on_reaction_add(const Json& d);
    void on_interaction_create(const Json& d);
    // ── READY (ClientReady) ──
    void on_ready(const Json& d);

    // ── ThreadCreate → フロー起動 ──

    struct ForumStarter {
        std::string guildId;
        std::string theme;
        std::vector<std::string> appliedTagNames;
        std::string starterContent;
        std::vector<std::string> specAttachmentUrls;
    };
    std::optional<Message> fetch_starter_with_retry(const std::string& thread_id);
    std::optional<ForumStarter> resolve_forum_starter(const Json& d);
    void post_components(const std::string& thread_id, const std::optional<std::string>& guild_id, const std::string& content, Json components, const char* what);
    void post_flow_pick_menu(const std::string& thread_id, const std::string& prefix = "");
    void post_flow_settings_menu(const std::string& thread_id);
    void launch_flow(const flow::StartForumFlowInput& input);
    static flow::StartForumFlowInput to_input(const BeginFlowInfo& info);
    void begin_flow(const BeginFlowInfo& info);
    bool start_from_settings(const std::string& thread_id, std::optional<double> rounds, std::optional<double> turns);
    void on_forum_thread_create(const Json& d);
    // ── MessageCreate ──
    bool has_review(const std::string& channel_id);
    void handle_message(const ForumMessageEvent& ev);
    bool maybe_handle_directive(const Message& msg);
    void route_discussion_message(const Message& msg, const Json& raw, bool starter);
    // ── MessageReactionAdd ──
    void handle_reaction(const ForumReactionEvent& ev);
    // ── InteractionCreate ──
    void handle_interaction(const ForumInteractionEvent& ev);
    void handle_select(Ix& ix, const Json& data, const std::string& custom_id);
    void handle_modal(Ix& ix, const Json& d, const Json& data, const std::string& custom_id, const std::string& channel_id);
    void handle_command(Ix& ix, const Json& d, const Json& data, const std::string& channel_id);
    // ── 外向き ──
    FinalizeForumPostResult finalize(const std::optional<std::string>& scene, const std::string& summary, const std::string& title);
    void react(const std::string& channel_id, const std::string& message_id, const std::string& emoji);
    SweepUnseededResult sweep(std::optional<int> limit_opt);
    StartExistingThreadResult start_existing_thread(const std::string& thread_id);
    static std::string url_encode_emoji(const std::string& emoji);
    void stop();
};

} // namespace di::discord_hook
