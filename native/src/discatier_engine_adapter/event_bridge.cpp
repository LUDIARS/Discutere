#include "discatier_engine_adapter/event_bridge.hpp"

#include <algorithm>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "persona_engine/learning/learner.hpp"

namespace di::discatier_engine_adapter {

namespace {

constexpr long long DEFAULT_POLL_MS = 1000;
const std::vector<std::string> DEFAULT_CLOSING{"HypothesisIntegrated", "HypothesisRejected"};
/** 1 回の poll で読む行のバイト上限 (DB の値も外部入力)。 */
constexpr sql::QueryLimits kPollLimits{kMaxEventsPerPoll + 1, 512u << 20};

/** payload の文字列項目 (`typeof p.k === "string"`)。 */
std::optional<std::string> str_field(const std::optional<Json>& p, const char* key) {
    if (!p || !p->is_object()) return std::nullopt;
    const auto it = p->find(key);
    if (it == p->end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

/** 空でない文字列項目。 */
std::optional<std::string> nonempty_field(const std::optional<Json>& p, const char* key) {
    auto v = str_field(p, key);
    if (v && v->empty()) return std::nullopt;
    return v;
}

/** speaker が「人間」か (persona:/ext: でも空でもない = Discord ユーザ)。 */
bool is_human_speaker(const std::optional<std::string>& speaker_id) {
    return speaker_id && !speaker_id->empty() && !str::starts_with(*speaker_id, "persona:") && !str::starts_with(*speaker_id, "ext:");
}

} // namespace

bool is_bound_scene(const std::optional<std::string>& scene) {
    return scene && (str::starts_with(*scene, "discord:") || str::starts_with(*scene, "web:"));
}

std::optional<Json> parse_event_payload(std::string_view raw) {
    auto v = parse_json(raw, kEventPayloadLimits);
    if (v && v->is_object()) return v;
    return std::nullopt;
}

BridgeEngine bridge_engine_of(const std::shared_ptr<persona_engine::PersonaEngineHandle>& engine) {
    BridgeEngine out;
    out.fireEvent = [engine](const persona_engine::engine::EngineEvent& e) { engine->fire_event(e); };
    out.resetSession = [engine](const std::string& session_id) { engine->reset_session(session_id); };
    out.personas = &engine->personas();
    out.rules = &engine->rules();
    return out;
}

EventBridge::EventBridge(core::Core& core, BridgeEngine engine, EventBridgeOptions options)
    : core_(core), engine_(std::move(engine)), options_(std::move(options)) {
    poll_ms_ = options_.pollMs.value_or(DEFAULT_POLL_MS);
    if (poll_ms_ < 1) poll_ms_ = 1;
    closing_ = options_.closingEventTypes.value_or(DEFAULT_CLOSING);
    // 起動時の最新 event を起点に (= 過去 event を流し直さない)
    const auto initial = core_.raw().get("SELECT COALESCE(MAX(created_at), 0) AS max_ts FROM events");
    last_seen_ = initial ? initial->num("max_ts") : 0;
}

EventBridge::~EventBridge() { stop(); }

void EventBridge::report(const std::string& error, const DiscatierEventRow* ev) {
    if (options_.onError) {
        try {
            options_.onError(error, ev);
        } catch (...) {
        }
        return;
    }
    log::warn("[event-bridge] " + error);
}

void EventBridge::start() {
    std::lock_guard lock(state_mutex_);
    if (timer_) return;
    stopped_ = false;
    std::weak_ptr<EventBridge> weak = weak_from_this();
    timer_ = global_scheduler().every(std::chrono::milliseconds(poll_ms_), [weak] {
        auto self = weak.lock();
        if (!self) return;
        {
            std::lock_guard l(self->state_mutex_);
            if (self->stopped_) return;
        }
        // poll (fire_event は LLM を待つ) は scheduler のスレッドでなく worker で走らせる。
        // キューが満杯なら今回は飛ばす (次の周期で読む)。
        background_executor().submit([weak] {
            auto bridge = weak.lock();
            if (!bridge) return;
            {
                std::lock_guard l(bridge->state_mutex_);
                if (bridge->stopped_) return;
            }
            try {
                bridge->poll_once();
            } catch (const std::exception& e) {
                bridge->report(e.what(), nullptr);
            }
        });
    });
}

void EventBridge::stop() {
    {
        std::lock_guard lock(state_mutex_);
        stopped_ = true;
        if (timer_) {
            global_scheduler().cancel(*timer_);
            timer_.reset();
        }
    }
    // 走行中の poll の終わりを待つ (この後に core / engine を閉じてよい)。
    std::lock_guard wait(poll_mutex_);
}

double EventBridge::last_seen_at() const {
    std::lock_guard lock(state_mutex_);
    return last_seen_;
}

int EventBridge::poll_once() {
    std::unique_lock poll_lock(poll_mutex_, std::try_to_lock);
    if (!poll_lock.owns_lock()) return 0;
    const double since = last_seen_at();
    const auto fetched = core_.raw().all(
        "SELECT id, event_type, payload_json, created_at FROM events WHERE created_at > ? ORDER BY created_at ASC, id ASC LIMIT ?",
        {since, static_cast<long long>(kMaxEventsPerPoll + 1)}, kPollLimits);
    std::vector<DiscatierEventRow> rows;
    rows.reserve(fetched.size());
    for (const auto& r : fetched) rows.push_back({r.str("id"), r.str("event_type"), r.str("payload_json"), r.num("created_at")});
    if (rows.size() > kMaxEventsPerPoll) {
        // 上限で切った末尾と同じ created_at の行は次の poll に回す (created_at > lastSeen で取りこぼさない)。
        rows.resize(kMaxEventsPerPoll);
        const double tail = rows.back().created_at;
        std::size_t keep = rows.size();
        while (keep > 0 && rows[keep - 1].created_at == tail) --keep;
        if (keep > 0) rows.resize(keep);
    }
    int dispatched = 0;
    for (const auto& row : rows) {
        try {
            dispatch_one(row);
            dispatched += 1;
            std::lock_guard lock(state_mutex_);
            if (row.created_at > last_seen_) last_seen_ = row.created_at;
        } catch (const std::exception& e) {
            report(e.what(), &row);
        } catch (...) {
            report("unknown error", &row);
        }
    }
    return dispatched;
}

void EventBridge::dispatch_one(const DiscatierEventRow& row) {
    const auto payload = parse_event_payload(row.payload_json);
    const auto ws = str_field(payload, "workspaceId");
    // 別 workspace の event は無視
    if (ws && *ws != options_.workspaceId) return;

    const auto session_id = resolve_session_id(row.event_type, payload);
    if (engine_.fireEvent) engine_.fireEvent({row.event_type, session_id, payload});

    // 人間の発言 → 進行中の議論に最優先で素早く応答させる (人間 > AI)。
    if (row.event_type == "UtteranceCreated") handle_human_utterance(payload);

    // 終結 event なら session カウンタ解放 + learning loop (#3-1)
    if (std::find(closing_.begin(), closing_.end(), row.event_type) != closing_.end()) {
        if (session_id && engine_.resetSession) engine_.resetSession(*session_id);
        const auto hypothesis_id = str_field(payload, "id");
        if (hypothesis_id) {
            try {
                if (!engine_.personas || !engine_.rules) throw std::runtime_error("engine の personas / rules が無いので結果を記録できない");
                persona_engine::learning::RecordOutcomeArgs args;
                args.personas = engine_.personas;
                args.rules = engine_.rules;
                args.hypothesisId = *hypothesis_id;
                args.outcome = row.event_type == "HypothesisIntegrated" ? "integrated" : "rejected";
                args.statement = str_field(payload, "statement");
                persona_engine::learning::record_hypothesis_outcome(args);
            } catch (const std::exception& e) {
                report(e.what(), &row);
            }
        }
    }
}

std::optional<std::string> EventBridge::resolve_session_id(const std::string& event_type, const std::optional<Json>& payload) {
    if (!payload) return std::nullopt;
    // 1) payload に sessionId が含まれていれば最優先
    if (auto s = nonempty_field(payload, "sessionId")) return s;
    // 2) DesignGap 関連: gapId に紐付く「議論 session」を引き当て (なければ作る)
    if (event_type == "DesignGapDetected" || event_type == "DesignGapUpdated") {
        if (auto gap_id = str_field(payload, "id")) return ensure_gap_discussion_session(*gap_id);
    }
    // 3) Hypothesis 関連: hypothesis 経由で gap を引き、さらに gap session に紐付け
    if (event_type == "HypothesisCreated" || event_type == "HypothesisProposed" || event_type == "HypothesisValidated" ||
        event_type == "HypothesisIntegrated" || event_type == "HypothesisRejected" || event_type == "HypothesisUpdated") {
        const auto hid = str_field(payload, "id");
        auto gap_id = str_field(payload, "designGapId");
        if (!gap_id && hid) gap_id = lookup_gap_id_for_hypothesis(*hid);
        // TS は `gapId ?? lookup` の後 `if (gapId)` (空文字は無し扱い)。
        if (gap_id && !gap_id->empty()) return ensure_gap_discussion_session(*gap_id);
    }
    return std::nullopt;
}

void EventBridge::handle_human_utterance(const std::optional<Json>& payload) {
    if (!payload) return;
    const auto speaker_id = str_field(payload, "speakerId");
    if (!is_human_speaker(speaker_id)) return;
    const std::string raw_content = str_field(payload, "rawContent").value_or("");
    if (str::trim(raw_content).empty()) return;
    const std::string src_session_id = str_field(payload, "sessionId").value_or("");
    if (src_session_id.empty()) return;

    const auto src = core_.raw().get("SELECT title, scene FROM sessions WHERE id = ?", {src_session_id});
    if (!src) return;
    // 既に議論 session 内の発言 (= ミラー済 or persona 議論場) は再処理しない。
    if (str::starts_with(src->str("title"), "discussion-of-gap:")) return;
    // トランスポート bound な scene (Discord / Web) の入口発話だけミラーする。
    const auto scene = src->opt_str("scene");
    if (!is_bound_scene(scene)) return;

    // 同じチャンネル (scene) の open な議論 session を引く。
    const auto discussion = core_.raw().get(
        "SELECT s.id AS id, s.title AS title\n"
        "           FROM sessions s\n"
        "           JOIN design_gaps g\n"
        "             ON g.id = SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1)\n"
        "          WHERE s.workspace_id = ?\n"
        "            AND s.title LIKE 'discussion-of-gap:%'\n"
        "            AND s.scene = ?\n"
        "            AND (g.status IS NULL OR g.status NOT IN ('closed', 'converged'))\n"
        "          ORDER BY s.started_at DESC LIMIT 1",
        {options_.workspaceId, *scene});
    if (!discussion) return;
    const std::string discussion_id = discussion->str("id");

    // 人間発言を議論 session にミラー (= 議論の一次参加者として記録)。
    core::repositories::UtteranceInput u;
    u.workspaceId = options_.workspaceId;
    u.sessionId = discussion_id;
    u.speakerId = speaker_id;
    u.rawContent = raw_content;
    u.postedAt = static_cast<double>(time::now_ms());
    core_.repos.utterance.create(u);

    // HumanUtterance を発火 → respond-to-human ルールが最優先で即応する。
    Json mirrored = *payload;
    mirrored["sessionId"] = discussion_id;
    mirrored["humanSpeakerId"] = *speaker_id;
    if (engine_.fireEvent) engine_.fireEvent({"HumanUtterance", discussion_id, mirrored});
}

std::optional<std::string> EventBridge::lookup_gap_id_for_hypothesis(const std::string& hypothesis_id) {
    const auto row = core_.raw().get("SELECT design_gap_id FROM hypotheses WHERE id = ?", {hypothesis_id});
    if (!row) return std::nullopt;
    return row->opt_str("design_gap_id");
}

std::string EventBridge::ensure_gap_discussion_session(const std::string& gap_id) {
    const std::string title = "discussion-of-gap:" + gap_id;
    const std::string scene = resolve_gap_discussion_scene(gap_id);
    const auto existing = core_.raw().get("SELECT id, scene FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1",
                                          {options_.workspaceId, title});
    if (existing) {
        const std::string id = existing->str("id");
        if (is_bound_scene(scene) && existing->opt_str("scene") != scene)
            core_.raw().run("UPDATE sessions SET scene = ?, updated_at = ? WHERE id = ?", {scene, time::now_ms(), id});
        return id;
    }
    core::repositories::SessionInput s;
    s.workspaceId = options_.workspaceId;
    s.title = title;
    s.startedAt = static_cast<double>(time::now_ms());
    s.scene = scene;
    return core_.repos.session.create(s);
}

std::string EventBridge::resolve_gap_discussion_scene(const std::string& gap_id) {
    const auto gap = core_.raw().get("SELECT evidence_json FROM design_gaps WHERE id = ?", {gap_id});
    const std::optional<Json> evidence = (gap && !gap->is_null("evidence_json")) ? parse_event_payload(gap->str("evidence_json")) : std::nullopt;
    if (const auto evidence_session_id = nonempty_field(evidence, "sessionId")) {
        const auto source_session = core_.raw().get("SELECT scene FROM sessions WHERE id = ?", {*evidence_session_id});
        // 入口発話の session が transport bound (Discord / Web) ならその scene を継承する。
        if (source_session) {
            const auto scene = source_session->opt_str("scene");
            if (is_bound_scene(scene)) return *scene;
        }
    }
    const auto guild_id = nonempty_field(evidence, "guildId");
    const auto channel_id = nonempty_field(evidence, "channelId");
    // Web チャット由来 (guildId="web") は web:<room> に、それ以外は discord:<guild>/<channel>。
    if (guild_id && *guild_id == "web" && channel_id) return "web:" + *channel_id;
    if (guild_id && channel_id) return "discord:" + *guild_id + "/" + *channel_id;
    return "gap:" + gap_id;
}

std::shared_ptr<EventBridge> create_event_bridge(core::Core& core, BridgeEngine engine, EventBridgeOptions options) {
    return std::make_shared<EventBridge>(core, std::move(engine), std::move(options));
}

} // namespace di::discatier_engine_adapter
