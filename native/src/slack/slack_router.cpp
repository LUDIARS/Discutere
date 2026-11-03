// src/slack/slack-router.ts の移植。
#include "slack/slack_router.hpp"

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "base/executor.hpp"
#include "base/log.hpp"
#include "slack/slack_entry.hpp"

namespace di::slack {

namespace {

/** thread_ts → 値 の上限つき表 (古いものから捨てる)。 */
template <class T>
class PendingTable {
public:
    void set(const std::string& key, T value) {
        if (!map_.count(key)) order_.push_back(key);
        map_[key] = std::move(value);
        while (map_.size() > kMaxPendingThreads && !order_.empty()) {
            map_.erase(order_.front());
            order_.pop_front();
        }
        if (order_.size() > kMaxPendingThreads * 4) {
            std::deque<std::string> next;
            for (const auto& k : order_)
                if (map_.count(k)) next.push_back(k);
            order_ = std::move(next);
        }
    }
    std::optional<T> take(const std::string& key) {
        const auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        T v = std::move(it->second);
        map_.erase(it);
        return v;
    }

private:
    std::map<std::string, T> map_;
    std::deque<std::string> order_;
};

struct PendingPick {
    std::string channel;
    std::string theme;
};

/** flow-pick 後・進行量選択待ちの保留 (thread_ts → {channel, theme, flow})。 */
struct PendingFlow {
    std::string channel;
    std::string theme;
    flow::FlowKind flow;
};

struct RouterState {
    SlackRouterConfig cfg;
    std::set<std::string> channels;
    std::mutex m;
    // flow-pick 待ち (議論タイプ未選択): thread_ts → theme。
    PendingTable<PendingPick> picks;
    // 進行量待ち (discussion/improvement): thread_ts → PendingFlow。
    PendingTable<PendingFlow> settings;
};

/** message event から文字列を安全に取り出す。 */
std::optional<std::string> str_of(const Json& obj, const char* key) {
    if (!obj.is_object()) return std::nullopt;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

void launch(const std::shared_ptr<RouterState>& st, StartSlackFlowInput input) {
    const std::string thread_ts = input.threadTs;
    auto task = [st, input = std::move(input)]() { start_slack_flow(input, st->cfg.deps, st->cfg.hooks); };
    const bool ok = st->cfg.submit ? st->cfg.submit(task) : background_executor().submit(task);
    if (!ok) log::warn("  slack-router: 実行キューが満杯のためフローを起動できません (thread=" + thread_ts + ")");
}

void on_event(const std::shared_ptr<RouterState>& st, const Json& event) {
    if (str_of(event, "type") != std::optional<std::string>("message")) return;
    if (event.contains("bot_id") && json_truthy(event["bot_id"])) return;  // bot 自身の投稿は無視
    const auto channel = str_of(event, "channel");
    if (!channel || !st->channels.count(*channel)) return;  // 設定チャンネル外は無視

    const auto ts = str_of(event, "ts");
    const auto thread_ts = str_of(event, "thread_ts");
    const std::string text = str_of(event, "text").value_or("");

    // ── 壁打ち継続中スレッドへの返信なら submit_user に流す ──
    if (thread_ts && has_slack_sparring_session(*thread_ts)) {
        handle_slack_flow_reply(*thread_ts, *channel, text, st->cfg.deps, st->cfg.hooks);
        return;
    }

    // ── スレッド開始 (トップレベル投稿) なら議論タイプ選択を出す ──
    if (is_thread_starter(event) && ts) {
        {
            std::lock_guard lock(st->m);
            st->picks.set(*ts, PendingPick{*channel, text});
        }
        try {
            PostMessageArgs a;
            a.channel = *channel;
            a.threadTs = *ts;  // スレッド (= ts) に選択 UI を出す
            a.text = "議論タイプを選択してください";
            a.blocks = build_flow_pick_blocks();
            post_message(st->cfg.deps.botToken, a, st->cfg.deps.webApi);
        } catch (const std::exception& e) {
            log::warn(std::string("  slack-router: flow-pick post 失敗: ") + e.what());
        }
    }
}

void on_interactive(const std::shared_ptr<RouterState>& st, const Json& payload) {
    if (str_of(payload, "type") != std::optional<std::string>("block_actions")) return;
    const auto action_id = first_action_id(payload);
    const auto value = first_selected_value(payload);
    if (!action_id || action_id->empty() || !value || value->empty()) return;
    // interactive payload から channel id / thread_ts を安全に取り出す。
    std::optional<std::string> channel;
    if (payload.contains("channel")) channel = str_of(payload["channel"], "id");
    std::optional<std::string> thread_ts;
    if (payload.contains("message")) {
        thread_ts = str_of(payload["message"], "thread_ts");
        if (!thread_ts) thread_ts = str_of(payload["message"], "ts");
    }
    if (!channel || channel->empty() || !thread_ts || thread_ts->empty()) return;

    // ── 議論タイプ選択 ──
    if (*action_id == SLACK_FLOW_PICK_ACTION) {
        const auto flow = flow::parse_flow_kind(std::string_view(*value));
        if (!flow) return;
        std::string theme;
        {
            std::lock_guard lock(st->m);
            if (auto p = st->picks.take(*thread_ts)) theme = p->theme;
            if (*flow == "discussion" || *flow == "improvement") st->settings.set(*thread_ts, PendingFlow{*channel, theme, *flow});
        }
        if (*flow == "discussion" || *flow == "improvement") {
            // 進行量選択待ちに移す。
            try {
                PostMessageArgs a;
                a.channel = *channel;
                a.threadTs = *thread_ts;
                a.text = "進行量を選んで開始してください";
                a.blocks = build_flow_settings_blocks();
                post_message(st->cfg.deps.botToken, a, st->cfg.deps.webApi);
            } catch (const std::exception& e) {
                log::warn(std::string("  slack-router: flow-settings post 失敗: ") + e.what());
            }
        } else {
            // learning / sparring は即起動。
            launch(st, StartSlackFlowInput{*channel, *thread_ts, theme, *flow, {}, std::nullopt, std::nullopt, std::nullopt});
        }
        return;
    }

    // ── 進行量選択 → discussion/improvement を起動 ──
    if (*action_id == SLACK_FLOW_SETTINGS_ACTION) {
        std::optional<PendingFlow> pending;
        {
            std::lock_guard lock(st->m);
            pending = st->settings.take(*thread_ts);
        }
        if (!pending) return;
        const auto preset = parse_settings_preset(*value);
        launch(st, StartSlackFlowInput{pending->channel, *thread_ts, pending->theme, pending->flow, {}, preset.rounds, preset.turnsPerRound,
                                       std::nullopt});
    }
}

} // namespace

SlackRouter create_slack_router(SlackRouterConfig cfg) {
    auto st = std::make_shared<RouterState>();
    st->channels.insert(cfg.channelIds.begin(), cfg.channelIds.end());
    st->cfg = std::move(cfg);
    SlackRouter r;
    r.onEvent = [st](const Json& event) { on_event(st, event); };
    r.onInteractive = [st](const Json& payload) { on_interactive(st, payload); };
    return r;
}

} // namespace di::slack
