#include "flow/sparring.hpp"

#include <algorithm>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "flow/conclusion.hpp"
#include "flow/cost_logger.hpp"
#include "flow/persona_pool.hpp"
#include "flow/round_summary.hpp"
#include "flow/sparring_commands.hpp"

namespace di::flow {

namespace {
constexpr const char* FLOW = "sparring";
} // namespace

Json to_json(const SparringTurnResult& r) {
    Json o = Json::object();
    o["kind"] = r.kind;
    if (r.kind == "responses") {
        o["user"] = r.user ? to_json(*r.user) : Json(nullptr);
        Json list = Json::array();
        for (const auto& u : r.utterances) list.push_back(to_json(u));
        o["utterances"] = std::move(list);
    } else if (r.kind == "round-summary") {
        o["round"] = r.round;
        o["summary"] = r.summary;
        o["aufhebung"] = r.aufhebung;
    } else {
        o["reason"] = r.reason;
        o["conclusion"] = r.conclusion;
        o["concluded"] = r.concluded;
    }
    return o;
}

SparringSession::SparringSession(std::string theme, std::vector<FlowTag> tags, SparringDeps deps)
    : session_id_(crypto::uuid_v4()), theme_(std::move(theme)), tags_(std::move(tags)), deps_(std::move(deps)) {
    if (!deps_.llm) throw std::invalid_argument("SparringSession: llm is required");
}

std::string SparringSession::paper_id() const {
    std::lock_guard lock(mutex_);
    return paper_id_;
}

WarnFn SparringSession::warn_fn() const {
    return deps_.warn ? deps_.warn : WarnFn([](const std::string& m) { di::log::warn("[sparring/warn] " + m); });
}

Rng SparringSession::rng_fn() const { return deps_.rng ? deps_.rng : Rng([] { return crypto::random_unit(); }); }

void SparringSession::start() {
    std::lock_guard lock(mutex_);
    const auto& cfg = config::get_config();
    const bool is_local = cfg.llm.backend == "local";
    const WarnFn log = deps_.log ? deps_.log : WarnFn([](const std::string& m) { di::log::info("[sparring] " + m); });
    const WarnFn warn = warn_fn();

    InvestigateArgs ia;
    ia.theme = theme_;
    ia.tags = tags_;
    if (deps_.gamesDir) ia.gamesDir = *deps_.gamesDir;
    ia.youtubeSearch = deps_.youtubeSearch;
    ia.youtubeMaxComments = static_cast<int>(cfg.flow.youtubeMaxComments);
    ia.warn = warn;
    investigation_ = investigate_theme(ia);

    const std::string supplement = paper_supplement(tags_);
    NewPaper np;
    np.sessionId = session_id_;
    np.theme = theme_;
    np.tags = tags_;
    np.mechanics = investigation_->mechanics;
    np.supplement = supplement;
    paper_id_ = persist_paper(np, FLOW);
    paper_ = DiscussionPaper{};
    paper_.paperId = paper_id_;
    paper_.sessionId = session_id_;
    paper_.theme = theme_;
    paper_.tags = tags_;
    paper_.mechanics = investigation_->mechanics;
    paper_.supplement = supplement;

    const std::string default_model = cfg.llm.model.value_or("claude-haiku-4-5-20251001");
    // G: 相手ペルソナ指定があればプールから解決して相手に充てる。未指定/未解決なら従来生成。
    std::vector<FlowPersona> opponents;
    for (const auto& ref : deps_.opponentPersonaIds) {
        const auto found = find_pool_persona(ref);
        // stance はセッション固定 (respec 01)。複数指定時は pro/con を交互に割り当てて対立を保つ。
        if (found) {
            ToFlowPersonaOpts to;
            to.role = "debater";
            to.defaultModel = default_model;
            to.isLocal = is_local;
            to.stance = opponents.size() % 2 == 0 ? "pro" : "con";
            opponents.push_back(to_flow_persona(*found, to));
        } else {
            warn("壁打ち相手「" + ref + "」がプールに見つかりません (スキップ)");
        }
    }
    if (!opponents.empty()) {
        personas_ = opponents;
        std::vector<std::string> names;
        for (const auto& p : opponents) names.push_back(p.name);
        log("壁打ち相手 (指定): " + str::join(names, ", "));
    } else {
        GeneratePersonasArgs ga;
        ga.count = static_cast<int>(cfg.flow.personaCount);
        ga.defaultModel = default_model;
        ga.isLocal = is_local;
        ga.rng = rng_fn();
        personas_ = generate_flow_personas(ga);
    }
    synthetic_opinions_ = std::find(tags_.begin(), tags_.end(), "機密") != tags_.end() ? synthesize_opinions(investigation_->mechanics)
                                                                                       : std::vector<std::string>{};
    started_ = true;
    log("壁打ち開始: \"" + theme_ + "\" (ペルソナ " + std::to_string(personas_.size()) + " 人, 上限 " +
        js_number_to_string(cfg.flow.sparringMaxTurns) + " ターン)");
}

SparringTurnResult SparringSession::submit_user(const std::string& text) {
    std::lock_guard lock(mutex_);
    if (ended_.load()) {
        SparringTurnResult r;
        r.kind = "ended";
        r.reason = "command";
        r.conclusion = "(終了済み)";
        return r;
    }
    if (!started_) throw std::logic_error("SparringSession: start() has not completed");

    const auto command = detect_sparring_command(text);
    if (command == "end") return finalize("command");
    if (command == "summarize") return close_round();

    // ── 通常発話: ユーザ発話を記録し AI が応答 ──────────────────────────────
    const auto user_record = record_user(text);
    const auto& cfg = config::get_config();
    const int responses_per_turn = std::max(1, deps_.responsesPerTurn.value_or(1));
    SparringTurnResult out;
    out.kind = "responses";
    out.user = user_record;
    for (int i = 0; i < responses_per_turn; ++i) {
        // 暴走ガード: 上限到達で安全に打ち切り (結論まとめへ)
        if (static_cast<double>(ai_turn_count_) >= cfg.flow.sparringMaxTurns) return finalize("guard");
        if (auto u = generate_response()) out.utterances.push_back(std::move(*u));
    }
    return out;
}

FlowUtteranceRecord SparringSession::record_user(const std::string& text) {
    FlowUtteranceRecord record;
    record.id = crypto::uuid_v4();
    record.sessionId = session_id_;
    record.paperId = paper_id_;
    record.round = round_;
    record.turn = static_cast<int>(round_utterances_.size());
    record.personaId = "user";
    record.personaName = "あなた";
    record.role = "user";
    record.stance = "neutral";
    record.text = str::trim(text);
    record.isError = false;
    persist_utterance(record);
    round_utterances_.push_back({record.personaName, record.text});
    all_utterances_.push_back(record);
    if (deps_.onUtterance) deps_.onUtterance(record);
    return record;
}

std::optional<FlowUtteranceRecord> SparringSession::generate_response() {
    const Rng rng = rng_fn();
    const WarnFn warn = warn_fn();
    const FlowPersona persona = pick_random_persona(personas_, rng);
    const FlowStance stance = persona_stance(persona);

    std::vector<ContextVoice> user_voices = deps_.listExternalVoices ? deps_.listExternalVoices({theme_}, 5) : std::vector<ContextVoice>{};
    if (investigation_ && investigation_->youtubeUsed && !investigation_->youtubeComments.empty()) {
        const std::size_t n = std::min<std::size_t>(3, investigation_->youtubeComments.size());
        for (std::size_t i = 0; i < n; ++i) user_voices.push_back(ContextVoice{investigation_->youtubeComments[i], "youtube", std::nullopt});
    }

    BuildPersonaPaperArgs pa;
    pa.paper = paper_;
    pa.persona = persona;
    pa.stance = stance;
    pa.currentRoundUtterances = round_utterances_;
    pa.userVoices = user_voices;
    pa.syntheticOpinions = synthetic_opinions_;
    const auto persona_paper = build_persona_paper(pa);
    const std::string prompt = paper_to_prompt(persona_paper, stance, persona);

    CostLogContext c;
    c.flow = FLOW;
    c.sessionId = session_id_;
    c.round = round_;
    c.turn = static_cast<double>(round_utterances_.size());
    c.role = persona.role;
    c.persona = persona.name;
    c.location = persona.role == "facilitator" ? "facilitator" : "utterance";
    CostLoggedClient logged(*deps_.llm, c);
    persona_engine::LLMInvokeArgs ia;
    ia.prompt = prompt;
    ia.model = persona.model;
    const auto result = logged.invoke(ia);

    std::string text_out;
    bool is_error = false;
    if (!result.ok) {
        text_out = "[エラー: " + result.error + "]";
        is_error = true;
        warn("壁打ち応答 LLM エラー: " + result.error);
    } else {
        text_out = str::trim(result.text);
        if (text_out.empty()) return std::nullopt;  // 空応答はターンに数えない
    }

    FlowUtteranceRecord record;
    record.id = crypto::uuid_v4();
    record.sessionId = session_id_;
    record.paperId = paper_id_;
    record.round = round_;
    record.turn = static_cast<int>(round_utterances_.size());
    record.personaId = persona.id;
    record.personaName = persona.name;
    record.role = persona.role;
    record.stance = stance;
    record.text = text_out;
    record.isError = is_error;
    persist_utterance(record);
    round_utterances_.push_back({persona.name, text_out});
    all_utterances_.push_back(record);
    ai_turn_count_ += 1;
    if (deps_.onUtterance) deps_.onUtterance(record);
    return record;
}

SparringTurnResult SparringSession::close_round() {
    RoundSummaryInput si;
    si.theme = theme_;
    si.sessionId = session_id_;
    si.paperId = paper_id_;
    si.round = round_;
    si.utterances = round_utterances_;
    si.stockedAufhebung = all_aufhebung_;
    si.warn = warn_fn();
    si.flow = FLOW;
    const auto summary = summarize_round(*deps_.llm, si);
    all_aufhebung_.insert(all_aufhebung_.end(), summary.newAufhebung.begin(), summary.newAufhebung.end());
    paper_.rounds.push_back({round_, summary.summary, summary.newAufhebung});

    SparringTurnResult out;
    out.kind = "round-summary";
    out.round = round_;
    out.summary = summary.summary;
    out.aufhebung = summary.newAufhebung;
    round_ += 1;
    round_utterances_.clear();
    return out;
}

SparringTurnResult SparringSession::finalize(const std::string& reason) {
    const WarnFn warn = warn_fn();
    // 現ラウンドに未締めの発話があれば先にサマライズ (止揚を結論材料に)
    if (!round_utterances_.empty()) {
        try {
            RoundSummaryInput si;
            si.theme = theme_;
            si.sessionId = session_id_;
            si.paperId = paper_id_;
            si.round = round_;
            si.utterances = round_utterances_;
            si.stockedAufhebung = all_aufhebung_;
            si.warn = warn;
            si.flow = FLOW;
            const auto s = summarize_round(*deps_.llm, si);
            all_aufhebung_.insert(all_aufhebung_.end(), s.newAufhebung.begin(), s.newAufhebung.end());
        } catch (const std::exception& e) {
            warn(std::string("終了時サマリ失敗: ") + e.what());
        }
    }

    GenerateConclusionArgs ca;
    ca.theme = theme_;
    ca.sessionId = session_id_;
    ca.paperId = paper_id_;
    ca.allUtterances = all_utterances_;
    ca.allAufhebung = all_aufhebung_;
    // 「まとめて」で締めたラウンドの要約を結論入力に含める (respec 04)。
    ca.rounds = paper_.rounds;
    // 壁打ちは投票しないため世論なし (空の評価)。結論は止揚 + 直近発話から生成。
    ca.warn = warn;
    ca.flow = FLOW;
    const auto conclusion = generate_conclusion(*deps_.llm, ca);

    ended_.store(true);
    SparringTurnResult out;
    out.kind = "ended";
    out.reason = reason;
    out.conclusion = conclusion.summary;
    out.concluded = conclusion.concluded;
    return out;
}

} // namespace di::flow
