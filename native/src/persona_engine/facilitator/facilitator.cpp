#include "persona_engine/facilitator/facilitator.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>
#include <utility>

#include "base/crypto.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "core/noise/exclusions.hpp"
#include "discord_hook/facilitator_directives.hpp"
#include "discord_hook/reactions.hpp"
#include "persona_engine/facilitator/js_value.hpp"
#include "persona_engine/index.hpp"

namespace di::persona_engine::facilitator {

namespace {

constexpr const char* kGapTitlePrefix = "discussion-of-gap:";

bool is_terminal_status(const std::optional<std::string>& status) {
    // TS: gap.status && ["closed","converged","dismissed"].includes(gap.status) (空文字は偽)
    return status && (*status == "closed" || *status == "converged" || *status == "dismissed");
}

bool is_human_speaker(const std::optional<std::string>& speaker_id) {
    return speaker_id && !speaker_id->empty() && !str::starts_with(*speaker_id, "persona:") &&
           !str::starts_with(*speaker_id, "ext:");
}

// 人間の意見はリアクションが無くても収束まとめで優先的に拾うためのスコア下駄。
constexpr std::int64_t HUMAN_OPINION_BASE = 5;

/** 発話一覧の読み出し上限 (DB の値も外部入力として課金する)。 */
constexpr sql::QueryLimits kUtteranceLimits{200'000, 256u << 20};

} // namespace

EvaluateResult evaluate(const SessionMetrics& metrics, const SessionState& state, double idle_gap_ms) {
    if (metrics.utteranceCount > state.lastUtteranceCount) {
        return {"active", SessionState{metrics.utteranceCount, true}};
    }
    const double idle_for = static_cast<double>(metrics.now - metrics.lastActivityAt);
    if (state.armed && idle_for >= idle_gap_ms) {
        SessionState next = state;
        next.armed = false;
        return {"intervene", next};
    }
    return {"wait", state};
}

const PersonaSeed& FACILITATOR_PERSONA() {
    static const PersonaSeed seed{
        FACILITATOR_PERSONA_ID,
        "進行役",
        "司会 結",
        "議論の活性化と収束を司る中立の進行役。",
        {"中立", "俯瞰", "要約", "合意形成"},
        "落ち着いた中立的な口調。 論点を整理し、 停滞時は新しい問いを投げ、 出揃ったら要約して締める。",
    };
    return seed;
}

Facilitator::Facilitator(FacilitatorDeps deps) : deps_(std::move(deps)) {
    if (!deps_.core) throw std::invalid_argument("facilitator: core required");
    if (!deps_.llm) throw std::invalid_argument("facilitator: llm required");
    if (!deps_.contextProvider) throw std::invalid_argument("facilitator: contextProvider required");
    if (!deps_.personas) throw std::invalid_argument("facilitator: personas required");
    if (!deps_.logger) deps_.logger = console_logger();

    // boot 時 options を既定 tuning とし、deps.tuning があれば tick ごとにライブ値を読む。tickMs だけは周期に焼くため固定。
    default_tuning_.tickMs = deps_.options.tickMs.value_or(30'000);
    default_tuning_.idleGapMs = deps_.options.idleGapMs.value_or(120'000);
    default_tuning_.maxPersonas = deps_.options.maxPersonas.value_or(20);
    default_tuning_.aufhebungTarget = deps_.options.aufhebungTarget.value_or(3);
    default_tuning_.convergePolicy = "default";

    // スコア参照のため reaction テーブルを冪等確保 + 止揚ストックテーブル。
    discord_hook::ensure_reaction_tables(raw());
    raw().exec(R"(CREATE TABLE IF NOT EXISTS aufhebung_stock (
       id TEXT PRIMARY KEY,
       gap_id TEXT NOT NULL,
       summary TEXT NOT NULL,
       created_at INTEGER NOT NULL
     ))");
    // ノイズ除外サイドカー表を冪等確保 (まとめ生成時に除外発話を弾くため)。
    core::noise::ensure_exclusion_table(raw());
    // 進行役への調整指示サイドカー表を冪等確保 (gapTopic で読み、expand/converge に効かせる)。
    discord_hook::ensure_facilitator_directive_table(raw());

    // 進行役 persona を登録 (収束のまとめ発言者)
    deps_.personas->insert_or_ignore(FACILITATOR_PERSONA());
}

Facilitator::~Facilitator() { timer_.stop(); }

runtime_settings::FacilitatorTuning Facilitator::get_tuning() const {
    return deps_.tuning ? deps_.tuning() : default_tuning_;
}

std::optional<std::string> Facilitator::model_for(const std::string& kind) const {
    lib::llm_gateway::RoutingInput input;
    input.kind = kind;
    auto m = resolve_tier_model(input, deps_.options.tierModels);
    return m ? m : deps_.options.model;
}

/**
 * open な議論 (discussion-of-gap session) を列挙。Discord 紐付け (scene=discord:*) に加え、
 * headless 議論 (scene=gap:*、自動シード等) も駆動対象に含める。
 */
std::vector<Facilitator::Discussion> Facilitator::list_active_discussions() {
    const auto rows = raw().all(R"(SELECT s.id AS sessionId, s.title AS title
           FROM sessions s
          WHERE s.workspace_id = ?
            AND s.title LIKE 'discussion-of-gap:%'
            AND (s.scene LIKE 'discord:%' OR s.scene LIKE 'gap:%'))",
                                {deps_.workspaceId}, kUtteranceLimits);
    std::vector<Discussion> out;
    for (const auto& r : rows) {
        const std::string gap_id = str::js_slice(r.str("title"), static_cast<std::int64_t>(str::js_length(kGapTitlePrefix)));
        const auto gap = raw().get("SELECT status FROM design_gaps WHERE id = ?", {gap_id});
        if (!gap) continue;
        if (is_terminal_status(gap->opt_str("status"))) continue;
        out.push_back({r.str("sessionId"), gap_id});
    }
    return out;
}

SessionMetrics Facilitator::metrics_for(const std::string& session_id, std::int64_t now) {
    const auto rows = raw().all(
        "SELECT speaker_id, posted_at FROM utterances WHERE session_id = ? AND id NOT IN (SELECT utterance_id FROM utterance_exclusions)",
        {session_id}, kUtteranceLimits);
    std::set<std::string> persona_ids;
    std::int64_t last_activity_at = 0;
    for (const auto& u : rows) {
        const auto sp = u.opt_str("speaker_id");
        if (sp && str::starts_with(*sp, "persona:")) persona_ids.insert(*sp);
        const auto posted = u.i64("posted_at");
        if (posted > last_activity_at) last_activity_at = posted;
    }
    SessionMetrics m;
    m.utteranceCount = static_cast<std::int64_t>(rows.size());
    m.distinctPersonas = static_cast<std::int64_t>(persona_ids.size());
    m.lastActivityAt = last_activity_at;
    m.now = now;
    return m;
}

std::vector<RecentUtterance> Facilitator::recent_utterances(const std::string& session_id, int limit) {
    auto rows = raw().all(
        "SELECT speaker_id, raw_content FROM utterances WHERE session_id = ? AND id NOT IN (SELECT utterance_id FROM utterance_exclusions) "
        "ORDER BY posted_at DESC LIMIT ?",
        {session_id, limit}, kUtteranceLimits);
    std::reverse(rows.begin(), rows.end());
    std::vector<RecentUtterance> out;
    out.reserve(rows.size());
    for (const auto& u : rows) out.push_back({speaker_label(u.opt_str("speaker_id")), u.str("raw_content")});
    return out;
}

std::string Facilitator::speaker_label(const std::optional<std::string>& speaker_id) {
    if (!speaker_id || speaker_id->empty()) return "参加者";
    if (str::starts_with(*speaker_id, "persona:")) {
        const std::string pid = speaker_id->substr(std::string_view("persona:").size());
        const auto p = deps_.personas->get(pid);
        return p ? p->display_name : pid;
    }
    if (str::starts_with(*speaker_id, "ext:")) return "外部の声";
    return "人間";  // Discord ユーザ (= 人間参加者)
}

std::string Facilitator::gap_topic(const std::string& gap_id) {
    const auto g = raw().get("SELECT title, description FROM design_gaps WHERE id = ?", {gap_id});
    const std::string base = g ? str::trim(g->str("title") + "\n" + g->str("description")) : std::string("(不明な議題)");
    // 参加者からの進行調整指示があれば topic 末尾に注入し、expand/converge/止揚判定の全 prompt に効かせる。
    const auto directives = discord_hook::list_facilitator_directives(raw(), deps_.workspaceId, gap_id);
    const auto block = discord_hook::format_directives_block(directives);
    return block.empty() ? base : base + "\n\n" + block;
}

std::vector<std::string> Facilitator::existing_persona_names(const std::string& session_id) {
    const auto rows = raw().all("SELECT DISTINCT speaker_id FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:%'",
                                {session_id}, kUtteranceLimits);
    std::vector<std::string> out;
    for (const auto& r : rows) out.push_back(speaker_label(r.opt_str("speaker_id")));
    return out;
}

std::vector<std::string> Facilitator::list_aufhebung(const std::string& gap_id) {
    const auto rows = raw().all("SELECT summary FROM aufhebung_stock WHERE gap_id = ? ORDER BY created_at ASC", {gap_id},
                                kUtteranceLimits);
    std::vector<std::string> out;
    for (const auto& r : rows) out.push_back(r.str("summary"));
    return out;
}

void Facilitator::stock_aufhebung(const std::string& gap_id, const std::string& summary) {
    raw().run("INSERT INTO aufhebung_stock (id, gap_id, summary, created_at) VALUES (?, ?, ?, ?)",
              {crypto::uuid_v4(), gap_id, summary, time::now_ms()});
}

/** session 内発話をスコア降順で上位 (まとめがスコアに引っ張られる)。人間発言は下駄あり。 */
std::vector<std::string> Facilitator::top_opinions(const std::string& session_id, std::size_t limit) {
    const auto rows = raw().all(
        "SELECT id, speaker_id, raw_content FROM utterances WHERE session_id = ? AND speaker_id IS NOT NULL AND id NOT IN (SELECT utterance_id "
        "FROM utterance_exclusions)",
        {session_id}, kUtteranceLimits);
    struct Scored {
        std::optional<std::string> speaker;
        std::string content;
        bool human = false;
        std::int64_t score = 0;
    };
    std::vector<Scored> scored;
    for (const auto& u : rows) {
        const auto sp = u.opt_str("speaker_id");
        const bool human = is_human_speaker(sp);
        if (!(sp && str::starts_with(*sp, "persona:")) && !human) continue;
        const std::int64_t score = discord_hook::get_opinion_score(raw(), u.str("id")) + (human ? HUMAN_OPINION_BASE : 0);
        if (score <= 0) continue;
        scored.push_back({sp, u.str("raw_content"), human, score});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return b.score < a.score; });
    if (scored.size() > limit) scored.resize(limit);
    std::vector<std::string> out;
    for (const auto& u : scored)
        out.push_back(speaker_label(u.speaker) + "「" + u.content + "」(+" + std::to_string(u.score) + (u.human ? " / 人間" : "") + ")");
    return out;
}

/** 止揚判定 → 新規なら stock。stock 後の総数を返す。 */
std::size_t Facilitator::judge_and_stock_aufhebung(const Discussion& d) {
    const auto stocked = list_aufhebung(d.gapId);
    const auto prompt = build_aufhebung_judge_prompt(gap_topic(d.gapId), recent_utterances(d.sessionId, 16), stocked);
    LLMInvokeArgs args;
    args.prompt = prompt.user;
    args.system = prompt.system;
    args.model = model_for("aufhebung");
    const auto res = deps_.llm->invoke(args);
    if (!res.ok) return stocked.size();
    try {
        const Json obj = extract_json_object(res.text);
        if (!obj.is_object()) return stocked.size();
        const auto it = obj.find("summary");
        const std::string summary = it != obj.end() && it->is_string() ? str::trim(it->get<std::string>()) : std::string();
        const auto found = obj.find("found");
        const bool is_found = found != obj.end() && found->is_boolean() && found->get<bool>();
        if (is_found && !summary.empty() && std::find(stocked.begin(), stocked.end(), summary) == stocked.end()) {
            stock_aufhebung(d.gapId, summary);
            deps_.logger->info(Json{{"gap_id", d.gapId}, {"summary", summary}}, "facilitator stocked aufhebung");
            return stocked.size() + 1;
        }
    } catch (const std::exception&) {
        // 判定失敗は無視
    }
    return stocked.size();
}

void Facilitator::expand(const Discussion& d) {
    const auto prompt = build_expand_prompt(gap_topic(d.gapId), existing_persona_names(d.sessionId), recent_utterances(d.sessionId, 12));
    LLMInvokeArgs args;
    args.prompt = prompt.user;
    args.system = prompt.system;
    args.model = model_for("facilitate");
    const auto res = deps_.llm->invoke(args);
    if (!res.ok) {
        deps_.logger->warn(Json{{"gap_id", d.gapId}, {"err", res.error}}, "facilitator expand llm failed");
        return;
    }
    Json gen;
    try {
        gen = extract_json_object(res.text);
    } catch (const std::exception& err) {
        deps_.logger->warn(Json{{"gap_id", d.gapId}, {"err", err.what()}}, "facilitator expand parse failed");
        return;
    }
    if (!gen.is_object()) return;
    const auto opening = gen.find("opening");
    if (opening == gen.end() || !opening->is_string() || opening->get<std::string>().empty()) return;

    const std::string persona_id = "dyn-" + crypto::uuid_v4().substr(0, 8);
    PersonaSeed seed;
    seed.id = persona_id;
    seed.name = js_string_or(gen, "name", "新視点");
    seed.display_name = js_string_or(gen, "display_name", "名無し");
    seed.description = js_string_or(gen, "description", "");
    if (const auto t = gen.find("traits"); t != gen.end() && t->is_array())
        for (const auto& v : *t) seed.traits.push_back(js_string(v));
    seed.speech_style = js_string_or(gen, "speech_style", "");
    deps_.personas->insert_or_ignore(seed);

    PostUtteranceInput post;
    post.workspaceId = deps_.workspaceId;
    post.sessionId = d.sessionId;
    post.text = opening->get<std::string>();
    post.byPersonaId = persona_id;
    deps_.contextProvider->post_utterance(post);
    Json meta{{"gap_id", d.gapId}, {"persona_id", persona_id}};
    if (const auto n = gen.find("name"); n != gen.end()) meta["name"] = *n;
    deps_.logger->info(meta, "facilitator expanded discussion");
}

void Facilitator::converge(const Discussion& d) {
    const auto prompt = build_converge_prompt(gap_topic(d.gapId), recent_utterances(d.sessionId, 40), list_aufhebung(d.gapId),
                                              top_opinions(d.sessionId, 5));
    LLMInvokeArgs args;
    args.prompt = prompt.user;
    args.system = prompt.system;
    args.model = model_for("converge");
    const auto res = deps_.llm->invoke(args);
    if (!res.ok) {
        deps_.logger->warn(Json{{"gap_id", d.gapId}, {"err", res.error}}, "facilitator converge llm failed");
        return;
    }
    std::string summary;
    try {
        const Json obj = extract_json_object(res.text);
        const bool has = obj.is_object() && obj.contains("summary") && obj["summary"].is_string();
        summary = has ? obj["summary"].get<std::string>() : res.text;
    } catch (const std::exception&) {
        summary = res.text;
    }
    PostUtteranceInput post;
    post.workspaceId = deps_.workspaceId;
    post.sessionId = d.sessionId;
    post.text = "【収束】\n" + summary;
    post.byPersonaId = FACILITATOR_PERSONA_ID;
    deps_.contextProvider->post_utterance(post);
    close_gap(d.gapId);
    // gap closed と session ライフサイクルを揃える (ダッシュボードの「進行中」に出続けないように)。
    const auto scene = end_converged_sessions(d.sessionId);
    {
        std::lock_guard lock(states_mutex_);
        states_.erase(d.sessionId);  // 収束したら管理終了 (= ファシリテーター消滅)
    }
    deps_.logger->info(Json{{"gap_id", d.gapId}}, "facilitator converged discussion (gap closed)");

    // 収束フック (フォーラム集約: スレッドを締めてまとめを転記)。失敗は議論を止めない。
    if (deps_.onConverged) {
        try {
            deps_.onConverged(FacilitatorConvergedEvent{d.gapId, d.sessionId, scene, gap_title(d.gapId), summary});
        } catch (const std::exception& err) {
            deps_.logger->warn(Json{{"gap_id", d.gapId}, {"err", err.what()}}, "facilitator onConverged hook failed");
        } catch (...) {
            deps_.logger->warn(Json{{"gap_id", d.gapId}, {"err", "unknown error"}}, "facilitator onConverged hook failed");
        }
    }
}

std::string Facilitator::gap_title(const std::string& gap_id) {
    const auto g = raw().get("SELECT title FROM design_gaps WHERE id = ?", {gap_id});
    return g && !g->is_null("title") ? g->str("title") : std::string("(無題の議論)");
}

/**
 * 収束した議論の session を ended_at で閉じる (admin の close と同じ scene 単位)。
 * intake session と discussion-of-gap session は同一 scene を共有するため両方終了する。
 */
std::optional<std::string> Facilitator::end_converged_sessions(const std::string& session_id) {
    const auto row = raw().get("SELECT scene FROM sessions WHERE id = ?", {session_id});
    std::optional<std::string> scene = row ? row->opt_str("scene") : std::nullopt;
    const auto now = time::now_ms();
    if (scene && !scene->empty()) {
        raw().run("UPDATE sessions SET ended_at = ?, updated_at = ? WHERE workspace_id = ? AND scene = ? AND ended_at IS NULL",
                  {now, now, deps_.workspaceId, *scene});
    } else {
        raw().run("UPDATE sessions SET ended_at = ?, updated_at = ? WHERE id = ? AND ended_at IS NULL", {now, now, session_id});
    }
    return scene;
}

void Facilitator::close_gap(const std::string& gap_id) {
    const auto g = raw().get("SELECT title, description FROM design_gaps WHERE id = ?", {gap_id});
    if (!g) return;
    core::repositories::DesignGapPatch patch;
    patch.workspaceId = deps_.workspaceId;
    patch.title = g->str("title");
    patch.description = g->opt_str("description");
    patch.status = "closed";
    deps_.core->repos.designGap.update(gap_id, patch);
}

/** idle 時の介入: 止揚判定 → たまっていれば収束、まだなら拡張。 */
void Facilitator::intervene(const Discussion& d, const SessionMetrics& metrics) {
    const auto t = get_tuning();
    const auto auf_count = judge_and_stock_aufhebung(d);
    // aufhebung-only ポリシーでは persona 数では収束させず、止揚が出るまで続ける。
    const bool persona_over =
        t.convergePolicy == "aufhebung-only" ? false : static_cast<double>(metrics.distinctPersonas) > t.maxPersonas;
    if (static_cast<double>(auf_count) >= t.aufhebungTarget || persona_over) {
        converge(d);
    } else {
        expand(d);
    }
}

void Facilitator::tick_once() {
    const auto now = time::now_ms();
    for (const auto& d : list_active_discussions()) {
        const auto metrics = metrics_for(d.sessionId, now);
        SessionState prev{metrics.utteranceCount, true};
        {
            std::lock_guard lock(states_mutex_);
            if (const auto it = states_.find(d.sessionId); it != states_.end()) prev = it->second;
        }
        const auto r = evaluate(metrics, prev, get_tuning().idleGapMs);
        {
            std::lock_guard lock(states_mutex_);
            states_[d.sessionId] = r.state;
        }
        try {
            if (r.mode == "intervene") intervene(d, metrics);
        } catch (const std::exception& err) {
            deps_.logger->warn(Json{{"gap_id", d.gapId}, {"err", err.what()}}, "facilitator action failed");
        }
    }
}

void Facilitator::start() {
    const auto tick_ms = static_cast<long long>(default_tuning_.tickMs);
    timer_.start(
        std::chrono::milliseconds(tick_ms),
        [this] {
            try {
                tick_once();
            } catch (const std::exception& err) {
                deps_.logger->warn(Json{{"err", err.what()}}, "facilitator tick failed");
            }
        },
        [this] { deps_.logger->warn(Json::object(), "facilitator tick skipped (executor queue full)"); });
}

void Facilitator::stop() { timer_.stop(); }

ConvergeNowResult Facilitator::converge_now(const std::string& session_id, const std::string& gap_id) {
    const auto gap = raw().get("SELECT status FROM design_gaps WHERE id = ?", {gap_id});
    if (!gap) return {false, std::string("gap not found")};
    const auto status = gap->opt_str("status");
    if (is_terminal_status(status)) return {false, "already " + *status};
    try {
        converge({session_id, gap_id});
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
    // converge は LLM 失敗時に早期 return する (gap を閉じない) ので、実際に closed になったかで成否を判定する。
    const auto after = raw().get("SELECT status FROM design_gaps WHERE id = ?", {gap_id});
    const auto st = after ? after->opt_str("status") : std::nullopt;
    if (st && (*st == "closed" || *st == "converged")) return {true, std::nullopt};
    return {false, std::string("まとめ生成に失敗しました (LLM 応答なし)。再試行してください")};
}

std::shared_ptr<Facilitator> create_facilitator(FacilitatorDeps deps) { return std::make_shared<Facilitator>(std::move(deps)); }

} // namespace di::persona_engine::facilitator
