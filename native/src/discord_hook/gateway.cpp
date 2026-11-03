// @spec Di管理フォーラムの受信境界
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

// ─── 実装 ────────────────────────────────────────────────────

namespace {

constexpr const char* kFlowPickPrompt = "🏷️ 議論タイプのタグが付いていません。どのフローで進めますか？ (議論/改善/学習/壁打ち)";
constexpr const char* kFlowSettingsPrompt =
    "⚙️ 進行量を選んで議論を開始してください (ラウンド数 × ターン数)。「数値を指定」で任意の値も設定できます。";
constexpr const char* kSettingsExpired = "この設定は期限切れです。スレッドを作り直してください。";
constexpr const char* kPickExpired = "この選択は期限切れです。スレッドを作り直してください。";
constexpr const char* kNoPaperForGap =
    "このチャンネルには調整待ちのディスカッションペーパーがありません。レビュー中のフォーラムスレッドで実行してください。";
constexpr int kInteractionUpdateMessage = 7;
constexpr int kInteractionModal = 9;

std::string what_of(const std::exception& e) { return e.what(); }

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

} // namespace

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

    ManagedForumIds forums() const {
        std::lock_guard lock(mu);
        return managed_ids;
    }

    /** 読み取り fetch (同一性だけをキャッシュする。取得失敗は例外 = 境界は拒否)。 */
    std::optional<ForumChannelRef> fetch_channel(const std::string& id) {
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

    FetchForumChannel fetcher() {
        return [this](const std::string& id) { return fetch_channel(id); };
    }

    bool managed_location(const std::optional<std::string>& guild_id, const std::string& channel_id,
                          const std::optional<ForumChannelRef>& channel = std::nullopt) {
        return is_managed_forum_location(ForumLocation{guild_id, channel_id, channel}, forums(), fetcher());
    }

    bool is_managed_scene(const std::optional<std::string>& scene) {
        const auto parsed = parse_discord_scene(scene);
        return parsed && managed_location(parsed->guildId, parsed->channelId);
    }

    bool submit_background(std::function<void()> task) {
        if (flow_live && flow_live->submitBackground) return flow_live->submitBackground(std::move(task));
        return background_executor().submit(std::move(task));
    }

    void sleep_for(std::chrono::milliseconds ms) {
        if (deps.sleep) deps.sleep(ms);
        else std::this_thread::sleep_for(ms);
    }

    // ── 振り分け (制御スレッド。重い仕事をしない) ──

    void enqueue(bool critical, const char* what, std::function<void()> task) {
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

    void handle_dispatch(const std::string& event, const Json& d) {
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

    void count_rejected(bool passed) {
        if (passed) return;
        std::lock_guard lock(mu);
        stats.gateRejected++;
    }

    // ── 門 (forum_event_gate) を通す ──

    void on_thread_create(const Json& d) {
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

    static ForumMessageEvent message_event(const Json& d) {
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

    void on_message_create(const Json& d) { count_rejected(gate->on_message_create(message_event(d))); }

    void on_reaction_add(const Json& d) {
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

    void on_interaction_create(const Json& d) {
        ForumInteractionEvent ev;
        ev.guild_id = json_opt_string(d, "guild_id");
        ev.channel_id = json_opt_string(d, "channel_id");
        const auto& data = json_object(d, "data");
        const auto type = json_int(d, "type", 0);
        if ((type == 3 || type == 5) && data.is_object()) ev.custom_id = json_opt_string(data, "custom_id");
        ev.raw = d;
        count_rejected(gate->on_interaction_create(ev));
    }

    // ── READY (ClientReady) ──

    void on_ready(const Json& d) {
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

    // ── ThreadCreate → フロー起動 ──

    struct ForumStarter {
        std::string guildId;
        std::string theme;
        std::vector<std::string> appliedTagNames;
        std::string starterContent;
        std::vector<std::string> specAttachmentUrls;
    };

    /** starter message を取得 (作成直後は未確定なことがあるので 1 回リトライ)。 */
    std::optional<Message> fetch_starter_with_retry(const std::string& thread_id) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                return api->get_message(thread_id, thread_id);
            } catch (...) {
                // 作成直後は未確定 → リトライ
            }
            sleep_for(std::chrono::milliseconds(500));
        }
        return std::nullopt;
    }

    /** フォーラム starter からテーマ + 適用タグ名 + guildId を解決する。管理forum ID境界の外 / starter が bot なら nullopt。 */
    std::optional<ForumStarter> resolve_forum_starter(const Json& d) {
        Channel thread;
        try {
            thread = channel_from_json(d);
        } catch (...) {
            return std::nullopt;
        }
        if (!managed_location(thread.guild_id, thread.id, forum_ref_from_channel(thread))) return std::nullopt;
        if (!thread.parent_id) return std::nullopt;
        std::optional<Channel> parent;
        try {
            parent = api->get_channel(*thread.parent_id);
        } catch (...) {
            return std::nullopt;  // 取得不可 → 判定不能
        }
        if (parent->type != channel_type::GuildForum) return std::nullopt;
        if (!thread.guild_id || thread.guild_id->empty()) return std::nullopt;
        ForumStarter s;
        s.guildId = *thread.guild_id;
        s.appliedTagNames = thread_applied_tag_names(thread, parent);
        const auto starter = fetch_starter_with_retry(thread.id);
        if (!starter) {
            log::warn("  discord-forum: starter message 取得不可 (thread=" + thread.id + ")");
            return std::nullopt;
        }
        if (starter->author.bot) return std::nullopt;
        // 議題はスレッド名を優先 (本文にゲーム名が無くても議題を固定する)。
        s.starterContent = str::trim(starter->content);
        const std::string name = thread.name ? str::trim(*thread.name) : std::string();
        s.theme = !name.empty() ? name : s.starterContent;
        if (s.theme.empty()) return std::nullopt;
        // テキスト系の添付ファイル URL を仕様書解析 (③) の材料にする (画像/バイナリは除外)。
        for (const auto& a : starter->attachments)
            if (is_text_like_attachment(a.filename, a.content_type)) s.specAttachmentUrls.push_back(a.url);
        return s;
    }

    void post_components(const std::string& thread_id, const std::optional<std::string>& guild_id, const std::string& content,
                         Json components, const char* what) {
        try {
            if (!managed_location(guild_id, thread_id)) return;
            api->create_message(thread_id, Json{{"content", content}, {"components", std::move(components)}});
        } catch (const std::exception& e) {
            log::warn(std::string("  discord-forum: ") + what + " menu 失敗 (thread=" + thread_id + "): " + what_of(e));
        }
    }

    /** 議論タイプ select メニューをスレッドに出す。 */
    void post_flow_pick_menu(const std::string& thread_id, const std::string& prefix = "") {
        std::optional<std::string> guild;
        {
            std::lock_guard lock(mu);
            if (auto p = pending_picks.get(thread_id)) guild = p->guildId;
        }
        post_components(thread_id, guild, prefix + kFlowPickPrompt, Json::array({build_flow_pick_menu(thread_id)}), "flow-pick");
    }

    /** 進行量設定 UI (プリセット select + 数値指定ボタン) をスレッドに出す。 */
    void post_flow_settings_menu(const std::string& thread_id) {
        std::optional<std::string> guild;
        {
            std::lock_guard lock(mu);
            if (auto p = pending_settings.get(thread_id)) guild = p->guildId;
        }
        Json rows = Json::array();
        for (auto& r : build_flow_settings_components(thread_id)) rows.push_back(std::move(r));
        post_components(thread_id, guild, kFlowSettingsPrompt, std::move(rows), "flow-settings");
    }

    /** フローの起動を背景キューへ (完走を待たない)。満杯なら選択メニューを出し直して再試行を促す。 */
    void launch_flow(const flow::StartForumFlowInput& input) {
        auto self = shared_from_this();
        const bool queued = submit_background([self, input] {
            if (!self->flow_live) return;
            flow::start_forum_flow(input, *self->flow_live, self->flow_hooks);
        });
        if (queued) return;
        log::error("  discord-forum: 背景キューが満杯のためフローを起動できない (thread=" + input.threadId + ")");
        {
            std::lock_guard lock(mu);
            pending_picks.set(input.threadId, PendingFlowPick{input.guildId, input.theme, input.tags, input.rounds, input.turnsPerRound,
                                                              input.opponentPersonaIds, input.specText, input.specAttachmentUrls});
        }
        post_flow_pick_menu(input.threadId, "⚠️ 混雑のため開始できませんでした。もう一度選んでください。\n");
    }

    static flow::StartForumFlowInput to_input(const BeginFlowInfo& info) {
        flow::StartForumFlowInput in;
        in.guildId = info.guildId;
        in.threadId = info.threadId;
        in.theme = info.theme;
        in.flow = info.flow;
        in.tags = info.tags;
        in.rounds = info.rounds;
        in.turnsPerRound = info.turnsPerRound;
        in.opponentPersonaIds = info.opponentPersonaIds;
        in.specText = info.specText;
        in.specAttachmentUrls = info.specAttachmentUrls;
        return in;
    }

    /**
     * フロー起動の入口 (item1)。discussion/improvement で進行量が本文未指定なら進行量設定 UI を出して待つ。
     * それ以外 (learning/sparring、または本文指定済み) は即起動。
     */
    void begin_flow(const BeginFlowInfo& info) {
        if (!flow_live) return;
        const bool wants_settings = (info.flow == "discussion" || info.flow == "improvement") && !info.rounds && !info.turnsPerRound;
        if (wants_settings) {
            {
                std::lock_guard lock(mu);
                pending_settings.set(info.threadId, PendingFlowSettings{info.guildId, info.theme, info.flow, info.tags, info.opponentPersonaIds,
                                                                        info.specText, info.specAttachmentUrls});
            }
            post_flow_settings_menu(info.threadId);
            return;
        }
        launch_flow(to_input(info));
    }

    /** 進行量設定 (select/modal) を受けて pending を起動する。期限切れなら false。 */
    bool start_from_settings(const std::string& thread_id, std::optional<double> rounds, std::optional<double> turns) {
        std::optional<PendingFlowSettings> pending;
        {
            std::lock_guard lock(mu);
            if (!flow_live || !pending_settings.has(thread_id)) return false;
            pending = pending_settings.take(thread_id);
        }
        BeginFlowInfo info{pending->guildId, thread_id, pending->theme, pending->flow, pending->tags, rounds, turns,
                           pending->opponentPersonaIds, pending->specText, pending->specAttachmentUrls};
        launch_flow(to_input(info));
        return true;
    }

    /** フォーラム starter を解析し、フロー起動 or タグ選択 UI を出す。 */
    void on_forum_thread_create(const Json& d) {
        if (!flow_live) return;
        const auto starter = resolve_forum_starter(d);
        if (!starter) return;
        const std::string thread_id = json_string(d, "id");
        const auto& p = deps.entryParsers;
        ParsedForumEntry entry = p.parseForumEntry ? p.parseForumEntry(starter->appliedTagNames) : ParsedForumEntry{};
        // 本文/タイトルの「ラウンド:N ターン:M」「相手:名前」記法で議論ごとの指定 (任意)。
        const ParsedRoundsTurns rt = p.parseRoundsTurns ? p.parseRoundsTurns(starter->theme) : ParsedRoundsTurns{};
        const auto opponents = p.parseOpponents ? p.parseOpponents(starter->theme) : std::nullopt;
        // starter 本文がタイトル (議題) と別物なら仕様書解析の材料にする (② 学習のみで使用)。
        std::optional<std::string> spec_text;
        if (!starter->starterContent.empty() && starter->starterContent != starter->theme) spec_text = starter->starterContent;
        std::optional<std::vector<std::string>> attachments;
        if (!starter->specAttachmentUrls.empty()) attachments = starter->specAttachmentUrls;
        if (entry.flow) {
            begin_flow(BeginFlowInfo{starter->guildId, thread_id, starter->theme, *entry.flow, entry.tags, rt.rounds, rt.turnsPerRound,
                                     opponents, spec_text, attachments});
            return;
        }
        // 議論タイプタグ無し → 選択 UI を出して待つ。
        {
            std::lock_guard lock(mu);
            pending_picks.set(thread_id, PendingFlowPick{starter->guildId, starter->theme, entry.tags, rt.rounds, rt.turnsPerRound,
                                                         opponents, spec_text, attachments});
        }
        post_flow_pick_menu(thread_id);
    }

    // ── MessageCreate ──

    bool has_review(const std::string& channel_id) {
        try {
            return flow::has_paper_review(channel_id);
        } catch (const std::exception& e) {
            log::warn("  discord-forum: レビュー状態の確認失敗: " + what_of(e));
            return false;
        }
    }

    void handle_message(const ForumMessageEvent& ev) {
        Message msg;
        try {
            msg = message_from_json(ev.raw);
        } catch (...) {
            return;  // 形の壊れた投稿は捨てる
        }
        if (msg.author.bot) return;
        // Discord のリプライは宛先に関係なく対象外。通常投稿の回答だけを受け付ける。
        if (msg.message_reference) return;

        // ゲーム感想チャンネル: カテゴリ「ゲーム感想」配下の投稿は議論にせず感想収集 (匿名)。
        if (deps.gameFeedback && deps.gameFeedback->enabled) {
            try {
                const Channel channel = api->get_channel(msg.channel_id);
                std::optional<Channel> parent;
                if (channel.parent_id) {
                    try {
                        parent = api->get_channel(*channel.parent_id);
                    } catch (...) {
                    }
                }
                if (const auto fb = extract_game_feedback(msg, channel, parent, deps.gameFeedback->categoryName)) {
                    try {
                        if (deps.onGameFeedback) deps.onGameFeedback(GameFeedbackInput{fb->gameTitle, fb->content, fb->createdAt});
                        try {
                            api->create_reaction(msg.channel_id, msg.id, "📝");
                        } catch (...) {
                        }
                    } catch (const std::exception& e) {
                        log::warn("  game-feedback: 収集失敗: " + what_of(e));
                    }
                    return;  // 感想は議論ルーティングに回さない
                }
            } catch (const std::exception& e) {
                log::warn("  game-feedback: チャンネル取得失敗: " + what_of(e));
            }
        }

        const bool starter = is_forum_starter_message(msg.id, msg.channel_id);
        // レビュー中のスレッドは調整/承認として扱う (メンションがあっても調整指示に回さない)。
        if (forum_enabled && flow_live && !starter && has_review(msg.channel_id)) {
            route_discussion_message(msg, ev.raw, starter);
            return;
        }
        if (!maybe_handle_directive(msg)) route_discussion_message(msg, ev.raw, starter);
    }

    /**
     * bot (@Discutere) へのメンションを「進行役への調整指示」として取り込む (門を通った管理フォーラムでのみ)。
     * @returns true なら調整指示として処理済 (通常ルーティングを行わない)。
     */
    bool maybe_handle_directive(const Message& msg) {
        std::string bot_id;
        {
            std::lock_guard lock(mu);
            bot_id = self_id;
        }
        if (bot_id.empty()) return false;
        bool mentioned = false;
        for (const auto& u : msg.mentions) mentioned = mentioned || u.id == bot_id;
        if (!mentioned) return false;
        const std::string text = strip_bot_mention(msg.content, bot_id);
        try {
            const auto result = handle_directive_message(DirectiveMessageInput{deps.router.workspaceId, msg.guild_id.value_or("dm"), msg.channel_id,
                                                                               text, msg.author.id.empty() ? std::nullopt : std::optional(msg.author.id)});
            try {
                post_discord_channel(*api, DiscordPostArgs{msg.channel_id, result.reply, msg.id});
            } catch (...) {
            }
        } catch (const std::exception& e) {
            log::warn("  discord-directive: failed: " + what_of(e));
        }
        return true;
    }

    /** 共通gate後の管理フォーラム内ルーティング。 */
    void route_discussion_message(const Message& msg, const Json& raw, bool starter) {
        // フォーラムスレッド内の後続投稿: (1) ペーパーレビュー中なら調整/承認、(2) 中断した議論のやり直し、(3) 壁打ちの相手発話。
        if (forum_enabled) {
            if (!starter && flow_live) {
                auto self = shared_from_this();
                const Message m = msg;
                const bool queued = submit_background([self, m] {
                    try {
                        const auto& fl = *self->flow_live;
                        const std::string guild = m.guild_id.value_or("dm");
                        // 受付チェックは編集の有無と別。準備中の回答も対象にし、LLM の処理完了を待たずに受付を知らせる。
                        ack_paper_review_reply(ackable_message(*self->api, m), self->has_review(m.channel_id));
                        if (flow::handle_paper_review_reply(m.channel_id, guild, m.content, fl, self->flow_hooks)) return;
                        // 再起動などで途中で止まった議論は「再開」返信で同じペーパーからやり直す。
                        if (flow::handle_interrupted_discussion_reply(m.channel_id, guild, m.content, fl, self->flow_hooks)) {
                            ack_paper_review_reply(ackable_message(*self->api, m), true);
                            return;
                        }
                        flow::handle_forum_flow_reply(m.channel_id, guild, m.content, fl, self->flow_hooks);
                    } catch (const std::exception& e) {
                        log::warn("  discord-forum: flow reply 失敗: " + what_of(e));
                    }
                });
                if (!queued) log::warn("  discord-forum: 背景キューが満杯のため返信を処理できない (thread=" + msg.channel_id + ")");
            }
            return;
        }

        // データクロール用チャンネル: 貼られた URL を外部データ取り込みに回す (議論取り込みはしない)。
        std::optional<std::string> parent_id;
        try {
            const auto ref = fetch_channel(msg.channel_id);
            if (ref && ref->is_thread) parent_id = ref->parent_id;
        } catch (...) {
        }
        bool crawl_target = false;
        {
            std::lock_guard lock(mu);
            crawl_target = crawl_ids.count(msg.channel_id) || (parent_id && crawl_ids.count(*parent_id));
        }
        if (deps.crawlDeps && crawl_target) {
            auto self = shared_from_this();
            const Message m = msg;
            const bool queued = submit_background([self, m] {
                try {
                    handle_crawl_message(*self->api, m, CrawlHandlerDeps{*self->deps.crawlDeps, std::nullopt, std::nullopt});
                } catch (const std::exception& e) {
                    log::warn("  discord-gateway: crawl 失敗: " + what_of(e));
                }
            });
            if (!queued) log::warn("  discord-gateway: 背景キューが満杯のため crawl を見送る");
            return;
        }

        const auto normalized = normalize_discord_inbound_message(raw);
        if (!normalized) return;
        try {
            const auto r = route_inbound_message(*normalized, msg.guild_id.value_or("dm"), deps.router, parent_id);
            // 取り込んだ全件ではなく「議論の種になった投稿」にだけ 👀 を付ける。
            if (r.ingested && r.seed) {
                auto api_ref = api;
                const std::string channel = msg.channel_id, id = msg.id;
                resolve_seed_in_background(
                    r.seed,
                    [api_ref, channel, id](bool started) {
                        if (!started) return;
                        try {
                            api_ref->create_reaction(channel, id, "👀");
                        } catch (...) {
                        }
                    },
                    deps.router.submitBackground);
            }
        } catch (const std::exception& e) {
            log::warn("  discord-gateway: message route failed: " + what_of(e));
        }
    }

    // ── MessageReactionAdd ──

    void handle_reaction(const ForumReactionEvent& ev) {
        try {
            if (ev.user_bot) return;
            const Json& d = ev.raw;
            const auto& emoji_obj = json_object(d, "emoji");
            std::string emoji = json_string(emoji_obj, "name");
            if (emoji.empty()) emoji = json_string(emoji_obj, "id");
            const std::string message_id = json_string(d, "message_id");
            const std::string channel_id = json_string(d, "channel_id");
            if (message_id.empty() || emoji.empty()) return;
            // ペーパーレビュー承認: ✅ をレビュー中スレッドに付けたら議論を開始する (スコアリングより優先)。
            if (emoji == "✅" && flow_live && has_review(channel_id)) {
                auto self = shared_from_this();
                const std::string guild = json_opt_string(d, "guild_id").value_or("dm");
                const bool queued = submit_background([self, channel_id, guild] {
                    try {
                        flow::handle_paper_review_approval(channel_id, *self->flow_live, self->flow_hooks, guild);
                    } catch (const std::exception& e) {
                        log::warn("  discord-forum: ペーパー承認 (reaction) 失敗: " + what_of(e));
                    }
                });
                if (!queued) log::error("  discord-forum: 背景キューが満杯のため承認を処理できない (thread=" + channel_id + ")");
                return;
            }
            if (deps.onReaction) deps.onReaction(ReactionInfo{message_id, channel_id, emoji, json_string(d, "user_id")});
        } catch (const std::exception& e) {
            log::warn("  discord-gateway: reaction handle failed: " + what_of(e));
        }
    }

    // ── InteractionCreate ──

    void handle_interaction(const ForumInteractionEvent& ev) {
        const Json& d = ev.raw;
        Ix ix(api, d);
        if (!ix.valid()) return;
        const auto type = json_int(d, "type", 0);
        const auto& data = json_object(d, "data");
        const std::string custom_id = json_string(data, "custom_id");
        const std::string channel_id = json_string(d, "channel_id");

        if (type == 3) {
            const auto component_type = json_int(data, "component_type", 0);
            // ボタン: 進行量「数値を指定」(flow-custom:<threadId>) → モーダル表示 / 続行・停止ボタン。
            if (component_type == 2) {
                if (const auto thread_id = parse_flow_custom_btn_custom_id(custom_id)) {
                    bool has = false;
                    {
                        std::lock_guard lock(mu);
                        has = pending_settings.has(*thread_id);
                    }
                    if (has) ix.quietly([&] { ix.show_modal(build_flow_settings_modal(*thread_id)); });
                    else ix.quietly([&] { ix.reply(kSettingsExpired, true); });
                    return;
                }
                if (debate_runner) {
                    try {
                        discussion::ButtonInteraction b{json_string(d, "id"), json_string(d, "token"), custom_id, json_opt_string(d, "channel_id")};
                        debate_runner->handle_button(b);
                    } catch (...) {
                    }
                }
                return;
            }
            if (component_type == 3) handle_select(ix, data, custom_id);
            return;
        }
        if (type == 5) {
            handle_modal(ix, d, data, custom_id, channel_id);
            return;
        }
        if (type != 2) return;
        handle_command(ix, d, data, channel_id);
    }

    void handle_select(Ix& ix, const Json& data, const std::string& custom_id) {
        const auto& values = json_array(data, "values");
        const std::string value = !values.empty() && values[0].is_string() ? values[0].get<std::string>() : std::string();
        // 進行量プリセット (flow-settings:<threadId>) → rounds/turns を決めて起動 (item1)。
        if (const auto settings_thread = parse_flow_settings_custom_id(custom_id)) {
            const auto preset = parse_settings_preset(value);
            const bool started = start_from_settings(*settings_thread, preset.rounds, preset.turnsPerRound);
            ix.quietly([&] { ix.update(started ? "✅ 進行量「" + value + "」で開始します" : std::string(kSettingsExpired)); });
            return;
        }
        // 議論タイプ select (flow-pick:<threadId>) — タグ無し投稿のフロー選択。
        const auto thread_id = parse_flow_pick_custom_id(custom_id);
        if (!thread_id) return;
        std::optional<PendingFlowPick> pending;
        {
            std::lock_guard lock(mu);
            pending = pending_picks.get(*thread_id);
        }
        const auto flow = flow::parse_flow_kind(std::optional<std::string_view>(value));
        if (!pending || !flow || !flow_live) {
            ix.quietly([&] { ix.reply(kPickExpired, true); });
            return;
        }
        {
            std::lock_guard lock(mu);
            pending_picks.take(*thread_id);
        }
        // フロー再提案からの選び直し (09): 議論/改善なら今のペーパーのまま続ける。
        bool continued = false;
        try {
            continued = flow::continue_paper_review_with_flow(*thread_id, *flow, *flow_live, pending->guildId);
        } catch (const std::exception& e) {
            log::warn("  discord-forum: ペーパー続行失敗 (thread=" + *thread_id + "): " + what_of(e));
        }
        if (continued) {
            ix.quietly([&] { ix.update("✅ 「" + value + "」を選択 (今のペーパーで続行)"); });
            return;
        }
        // 壁打ち/学習を選んだら、進行中のペーパーレビュー待ちは破棄して新タイプで起動する。
        try {
            flow::cancel_paper_review(*thread_id);
        } catch (...) {
        }
        ix.quietly([&] { ix.update("✅ 「" + value + "」を選択"); });
        begin_flow(BeginFlowInfo{pending->guildId, *thread_id, pending->theme, *flow, pending->tags, pending->rounds, pending->turnsPerRound,
                                 pending->opponentPersonaIds, pending->specText, pending->specAttachmentUrls});
    }

    void handle_modal(Ix& ix, const Json& d, const Json& data, const std::string& custom_id, const std::string& channel_id) {
        (void)channel_id;
        const auto fields = modal_field_values(data);
        auto field = [&](const char* id) {
            auto it = fields.find(id);
            return it == fields.end() ? std::string() : str::trim(it->second);
        };
        if (const auto gap = parse_paper_gap_modal_custom_id(custom_id)) {
            const std::string detail = field(paper_gap_field_ids::detail);
            if (detail.empty()) {
                ix.quietly([&] { ix.reply("不足事項の内容を入力してください。", true); });
                return;
            }
            if (!flow_live) {
                ix.quietly([&] { ix.reply("ペーパー調整機能が無効です (flowLive 未設定)。", true); });
                return;
            }
            if (!has_review(gap->channelId)) {
                ix.quietly([&] { ix.reply(kNoPaperForGap, true); });
                return;
            }
            PaperGapInstructionInput in;
            in.type = gap->type;
            const auto target = field(paper_gap_field_ids::target);
            const auto source = field(paper_gap_field_ids::source);
            in.target = target;
            in.detail = detail;
            in.source = source;
            const std::string instruction = format_paper_gap_review_instruction(in);
            try {
                ix.defer_ephemeral();
            } catch (...) {
                log::warn("  discord-paper-gap: modal failed id=" + json_string(d, "id"));
                return;
            }
            // ペーパーの調整は LLM を待つ: defer 済みなので背景で処理して editReply で結果を返す。
            auto self = shared_from_this();
            auto shared_ix = std::make_shared<Ix>(ix);
            const std::string guild = json_opt_string(d, "guild_id").value_or("dm");
            const std::string gap_channel = gap->channelId;
            const std::string interaction_id = json_string(d, "id");
            const bool queued = submit_background([self, shared_ix, guild, gap_channel, instruction, interaction_id] {
                try {
                    const bool handled = flow::handle_paper_review_reply(gap_channel, guild, instruction, *self->flow_live, self->flow_hooks);
                    shared_ix->edit_reply(handled ? "不足事項を受け付けました。ペーパーの更新結果をスレッドに投稿します。"
                                                  : "不足事項は受け付けましたが、調整待ちのペーパーに接続できませんでした。");
                } catch (const std::exception& e) {
                    log::warn("  discord-paper-gap: modal failed id=" + interaction_id + ": " + what_of(e));
                    shared_ix->quietly([&] { shared_ix->edit_reply("不足事項の反映に失敗しました。"); });
                }
            });
            if (!queued) ix.quietly([&] { ix.edit_reply("不足事項の反映に失敗しました。"); });
            return;
        }
        // 進行量モーダル送信 (flow-modal:<threadId>) → 任意の rounds/turns で起動 (item1)。
        const auto modal_thread = parse_flow_modal_custom_id(custom_id);
        if (!modal_thread) return;
        const auto rounds = positive_number_field(field("rounds"));
        const auto turns = positive_number_field(field("turns"));
        const bool started = start_from_settings(*modal_thread, rounds, turns);
        const std::string content =
            started ? "✅ 進行量 (ラウンド " + (rounds ? js_number_to_string(*rounds) : std::string("既定")) + " × ターン " +
                          (turns ? js_number_to_string(*turns) : std::string("既定")) + ") で開始します"
                    : std::string(kSettingsExpired);
        ix.quietly([&] { ix.reply(content, !started); });
    }

    void handle_command(Ix& ix, const Json& d, const Json& data, const std::string& channel_id) {
        const std::string name = json_string(data, "name");
        if (name == MECHANICS_CHECK_COMMAND_NAME || name == LEVEL_CHECK_COMMAND_NAME) {
            try {
                const SlashInteraction si = slash_interaction_from_json(d);
                // 3 秒の応答期限を守るため先に defer し、LLM の診断は背景で行う。
                RestInteractionResponder(*api, si).defer_ephemeral();
                auto self = shared_from_this();
                const bool queued = submit_background([self, si, name] {
                    try {
                        PreDeferredResponder responder(RestInteractionResponder(*self->api, si));
                        if (name == MECHANICS_CHECK_COMMAND_NAME) handle_mechanics_check_command(si, responder, self->deps.mechanicsCheck.get());
                        else handle_level_check_command(si, responder, self->deps.levelCheck.get());
                    } catch (...) {
                        // 利用者の入力やバックエンドの失敗をログに出さず、汎用ルータにも落とさない。
                        log::warn("  discord-gateway: diagnostic reply delivery failed");
                    }
                });
                if (!queued) {
                    try {
                        RestInteractionResponder(*api, si).edit_reply(DiagnosticReply{diagnostic_failure(nullptr), std::nullopt});
                    } catch (...) {
                        log::warn("  discord-gateway: diagnostic reply delivery failed");
                    }
                }
            } catch (...) {
                log::warn("  discord-gateway: diagnostic reply delivery failed");
            }
            return;
        }

        if (name == PAPER_GAP_COMMAND_NAME) {
            std::string raw_type;
            for (const auto& o : json_array(data, "options"))
                if (o.is_object() && json_string(o, "name") == "type" && o.contains("value") && o["value"].is_string())
                    raw_type = o["value"].get<std::string>();
            if (!is_paper_gap_type(raw_type)) {
                ix.quietly([&] { ix.reply("不足事項の種類が不正です。", true); });
                return;
            }
            if (!flow_live) {
                ix.quietly([&] { ix.reply("ペーパー調整機能が無効です (flowLive 未設定)。", true); });
                return;
            }
            if (!has_review(channel_id)) {
                ix.quietly([&] { ix.reply(kNoPaperForGap, true); });
                return;
            }
            ix.quietly([&] { ix.show_modal(build_paper_gap_modal(raw_type, channel_id)); });
            return;
        }

        // /debate: パーティ議論を開始 (非同期、ack だけ即返す)。
        if (name == "debate") {
            if (!debate_runner) {
                ix.quietly([&] { ix.reply("議論機能が無効です (config.discussion 未設定)", true); });
                return;
            }
            std::string topic;
            for (const auto& o : json_array(data, "options"))
                if (o.is_object() && json_string(o, "name") == "topic" && o.contains("value") && o["value"].is_string())
                    topic = o["value"].get<std::string>();
            ix.quietly([&] { ix.reply("🗣️ 議論を開始します: 「" + topic + "」", true); });
            auto self = shared_from_this();
            if (!submit_background([self, channel_id, topic] {
                    try {
                        if (self->debate_runner) self->debate_runner->start(channel_id, topic);
                    } catch (const std::exception& e) {
                        log::warn("  discord-gateway: debate 失敗: " + what_of(e));
                    }
                }))
                log::warn("  discord-gateway: 背景キューが満杯のため debate を開始できない");
            return;
        }

        RestSlashReplier replier(*api, json_string(d, "id"), json_string(d, "token"));
        try {
            handle_slash_interaction(d, deps.router, replier);
        } catch (...) {
            log::warn("  discord-gateway: interaction failed id=" + json_string(d, "id"));
        }
    }

    // ── 外向き ──

    FinalizeForumPostResult finalize(const std::optional<std::string>& scene, const std::string& summary, const std::string& title) {
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

    void react(const std::string& channel_id, const std::string& message_id, const std::string& emoji) {
        try {
            if (forums().empty()) return;
            const auto ref = fetch_channel(channel_id);
            if (!ref || !managed_location(ref->guild_id, channel_id, ref)) return;
            api->create_reaction(channel_id, message_id, emoji);
        } catch (...) {
            // best-effort: メッセージ削除済 / 権限不足等は握り潰す
        }
    }

    SweepUnseededResult sweep(std::optional<int> limit_opt) {
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

    static std::string url_encode_emoji(const std::string& emoji) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : emoji) {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
        return out;
    }

    void stop() {
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
};

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
