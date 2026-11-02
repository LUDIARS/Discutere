#include "flow/director.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/crypto.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "flow/conclusion.hpp"
#include "flow/db/connection.hpp"
#include "flow/facilitator_turn.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/paper_refine.hpp"
#include "flow/persona_cast.hpp"
#include "flow/persona_pool.hpp"
#include "flow/persona_setup.hpp"
#include "flow/persona_turn.hpp"
#include "flow/round_summary.hpp"
#include "flow/turn_scheduler.hpp"
#include "lib/sentiment_core/js_number.hpp"

namespace di::flow {

// ─── JSON ───────────────────────────────────────────────────────

Json to_json(const FlowUtteranceRecord& u) {
    Json o = Json::object();
    o["id"] = u.id;
    o["sessionId"] = u.sessionId;
    o["paperId"] = u.paperId;
    o["round"] = u.round;
    o["turn"] = u.turn;
    o["personaId"] = u.personaId;
    o["personaName"] = u.personaName;
    o["role"] = u.role;
    o["stance"] = u.stance;
    o["text"] = u.text;
    o["isError"] = u.isError;
    if (u.possessionName) o["possessionName"] = *u.possessionName;
    if (u.act) o["act"] = *u.act;
    if (u.targetId) o["targetId"] = *u.targetId;
    if (u.claimKind) o["claimKind"] = *u.claimKind;
    if (u.evidenceIds) o["evidenceIds"] = *u.evidenceIds;
    return o;
}

Json to_json(const VoteEvent& e) {
    Json tally = Json::object();
    for (const auto& [id, v] : e.tally) tally[id] = v;
    Json candidates = Json::array();
    for (const auto& c : e.candidates) candidates.push_back(Json{{"id", c.id}, {"personaName", c.personaName}});
    Json o = Json::object();
    o["round"] = e.round;
    o["kind"] = e.kind;
    o["tally"] = std::move(tally);
    o["winner"] = e.winner ? Json(*e.winner) : Json(nullptr);
    o["candidates"] = std::move(candidates);
    return o;
}

Json to_json(const FlowDirectorResult& r) {
    Json utterances = Json::array();
    for (const auto& u : r.utterances) utterances.push_back(to_json(u));
    Json o = Json::object();
    o["sessionId"] = r.sessionId;
    o["paperId"] = r.paperId;
    o["utterances"] = std::move(utterances);
    o["rounds"] = r.rounds;
    o["conclusion"] = r.conclusion;
    o["concluded"] = r.concluded;
    if (r.foresight) o["foresight"] = Json{{"proposal", r.foresight->proposal}, {"forecast", r.foresight->forecast}};
    return o;
}

// ─── 進行状態 ───────────────────────────────────────────────────

FlowRunState::Snapshot FlowRunState::snapshot() const {
    std::lock_guard lock(mutex_);
    return {phase_, round_, rounds_, turn_, utterances_, stop_.load()};
}
void FlowRunState::set_phase(std::string phase) {
    std::lock_guard lock(mutex_);
    phase_ = std::move(phase);
}
void FlowRunState::set_position(int round, int rounds, int turn) {
    std::lock_guard lock(mutex_);
    round_ = round;
    rounds_ = rounds;
    turn_ = turn;
}
void FlowRunState::set_utterances(int n) {
    std::lock_guard lock(mutex_);
    utterances_ = n;
}

// ─── 部品 ───────────────────────────────────────────────────────

double clamp_count(std::optional<double> value, double fallback, double max) {
    if (!value || !std::isfinite(*value)) return fallback;
    return std::max(1.0, std::min(max, std::floor(*value)));
}

bool is_vote_candidate(const FlowUtteranceRecord& u, int round) {
    return u.round == round && !u.isError && u.role != "facilitator";
}

void persist_utterance(const FlowUtteranceRecord& u) {
    auto db = db::get_flow_db();
    db->run(R"SQL(INSERT INTO flow_utterance
       (id, session_id, paper_id, round, turn, persona_id, persona_name, role, stance, text, is_error, possession_name, act, target_id,
        claim_kind, evidence_ids_json, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))SQL",
            {u.id, u.sessionId, u.paperId, u.round, u.turn, u.personaId, u.personaName, u.role, u.stance, u.text,
             u.isError ? 1 : 0, sql::Value(u.possessionName),
             // 会話行為 (respec 05)。rounds エンジンは nullopt → NULL (acts は dialectic 専用)。
             sql::Value(u.act), sql::Value(u.targetId),
             // 代理議論のみ (それ以外は NULL)。
             sql::Value(u.claimKind),
             u.evidenceIds && !u.evidenceIds->empty() ? sql::Value(json_stringify(Json(*u.evidenceIds))) : sql::Value(),
             time::now_ms()});
}

namespace {

/** 既定のラウンド世論決定 = 中立投票 (discussion.md step 6, respec 03 のレンズ/シャッフル込み)。 */
RoundEvaluator make_default_evaluator(Rng rng) {
    return [rng = std::move(rng)](const RoundEvalContext& ctx) {
        const auto& cfg = config::get_config();
        RunRoundVoteArgs a;
        a.theme = ctx.theme;
        a.sessionId = ctx.sessionId;
        a.round = ctx.round;
        a.voterCount = static_cast<int>(cfg.flow.voterCount);
        a.utterances = ctx.candidates;
        a.lenses = cfg.flow.voteLenses;
        a.rng = rng;
        a.warn = ctx.warn;
        return run_round_vote(*ctx.llm, a);
    };
}

/**
 * 憑依 (B, item4): テーマから嗜好を類推し、プール最近傍ペルソナを opinion 1 枠に「憑依」させる。
 * プールが空 / 一致なしなら no-op。投稿主体の代理は 1 体のみ (Q3)。
 */
void apply_theme_possession(std::vector<FlowPersona>& personas, const std::string& theme, const WarnFn& log, const WarnFn& warn) {
    try {
        const auto hits = select_possession_by_theme(theme, 1);
        if (!hits.empty()) {
            const auto& hit = hits[0];
            auto seat = std::find_if(personas.begin(), personas.end(), [](const auto& p) { return p.role == "opinion"; });
            if (seat != personas.end()) {
                seat->possession = PersonaPossession{hit.persona.name, describe_possession(hit.persona)};
                log("憑依: 「" + seat->name + "」が「" + hit.persona.name + "」の人物像を憑依 (cos=" +
                    lib::sentiment_core::js_to_fixed(hit.similarity, 3) + ")");
            }
        }
    } catch (const std::exception& e) {
        warn(std::string("憑依スキップ (") + e.what() + ")");
    }
}

/** ファシリテーター開幕に注入する前ラウンド文脈 (respec 02, A8)。round 1 は nullopt。 */
std::optional<PreviousRoundContext> build_previous_round_context(const std::vector<RoundSummary>& rounds,
                                                                 const std::vector<VoteResult>& evaluations,
                                                                 const std::vector<FlowUtteranceRecord>& all_utterances) {
    if (rounds.empty()) return std::nullopt;
    const auto& prev = rounds.back();
    PreviousRoundContext ctx;
    ctx.round = prev.round;
    ctx.summary = prev.summary;
    ctx.aufhebung = prev.aufhebung;
    if (!evaluations.empty() && evaluations.back().winner) {
        const std::string& w = *evaluations.back().winner;
        const auto it = std::find_if(all_utterances.begin(), all_utterances.end(), [&](const auto& u) { return u.id == w; });
        if (it != all_utterances.end()) ctx.winner = PreviousRoundContext::Winner{it->personaName, it->text};
    }
    return ctx;
}

/** 早期収束の判定 (respec 04, A6): 止揚 ≥ aufhebungTarget かつ 投票集中度 ≥ flow.convergeShare。 */
bool should_converge_early(std::size_t aufhebung_count, const std::optional<VoteResult>& last, const WarnFn& log) {
    const auto& cfg = config::get_config();
    const double n = static_cast<double>(aufhebung_count);
    if (n < cfg.facilitator.aufhebungTarget) return false;
    const std::string count = std::to_string(aufhebung_count);
    if (last && last->kind && *last->kind == "scores") {
        log("止揚 " + count + " 件が上限 (" + js_number_to_string(cfg.facilitator.aufhebungTarget) + ") に達したため早期収束");
        return true;
    }
    const double share = last && last->winnerShare ? *last->winnerShare : 0;
    const std::string share_s = lib::sentiment_core::js_to_fixed(share, 2);
    const std::string target_s = js_number_to_string(cfg.flow.convergeShare);
    if (share >= cfg.flow.convergeShare) {
        log("止揚 " + count + " 件 + 投票集中度 " + share_s + " ≥ " + target_s + " のため早期収束");
        return true;
    }
    log("止揚 " + count + " 件は上限に達したが、投票集中度 " + share_s + " < " + target_s + " (世論が割れている) — 続行");
    return false;
}

void check_stop(const std::shared_ptr<FlowRunState>& state) {
    if (state && state->stop_requested()) {
        state->set_phase("stopped");
        throw FlowStopped("flow stopped by request");
    }
}

} // namespace

FlowDirectorResult run_flow(persona_engine::LLMClient& llm, const std::string& theme, const std::vector<FlowTag>& tags,
                            const FlowRunOptions& options) {
    const auto& cfg = config::get_config();
    const WarnFn log = options.log ? options.log : WarnFn([](const std::string& m) { di::log::info("[flow] " + m); });
    const WarnFn warn = options.warn ? options.warn : WarnFn([](const std::string& m) { di::log::warn("[flow/warn] " + m); });
    const Rng rng = options.rng ? options.rng : Rng([] { return crypto::random_unit(); });
    const std::string& flow = options.flow;
    const auto& state = options.state;

    // 発話時の DB 参照キャッシュ (item5): 同一テーマ/語の外部の声 lookup をセッション内で 1 回に集約する。
    VoiceCache voice_cache(options.listExternalVoices);

    // ラウンド世論決定の戦略。既定は中立投票。改善フローは机上の機械スコアを注入する。
    const RoundEvaluator evaluate_round = options.evaluateRound ? options.evaluateRound : make_default_evaluator(rng);

    const std::string session_id = options.sessionId ? *options.sessionId : crypto::uuid_v4();
    const bool is_local = cfg.llm.backend == "local";
    // ラウンド/ターン数は議論ごとの指定を優先し、無ければ config 既定。暴走ガードでクランプ。
    const int rounds = static_cast<int>(clamp_count(options.rounds, cfg.flow.rounds, MAX_ROUNDS));
    const int turns_per_round = static_cast<int>(clamp_count(options.turnsPerRound, cfg.flow.turnsPerRound, MAX_TURNS_PER_ROUND));
    if (state) state->set_position(0, rounds, 0);

    // ── [1][2] 調査 + ディスカッションペーパー初期化 (flow-setup) ─────────────
    if (state) state->set_phase("setup");
    FlowSetupArgs setup;
    setup.theme = theme;
    setup.tags = tags;
    setup.flow = flow;
    setup.sessionId = session_id;
    setup.voiceCache = &voice_cache;
    setup.youtubeSearch = options.youtubeSearch;
    setup.gamesDir = options.gamesDir;
    setup.paperOverride = options.paperOverride;
    setup.log = log;
    setup.warn = warn;
    auto setup_result = setup_flow_paper(llm, setup);
    DiscussionPaper& paper = setup_result.paper;
    const std::string paper_id = setup_result.paperId;
    const std::vector<MechanicSummary> mechanics = paper.mechanics;
    const std::string supplement = paper.supplement;
    check_stop(state);

    // ── [3] ペルソナ生成 (stance セッション固定 + 憑依 + 価値軸/核主張) ─────────
    if (state) state->set_phase("cast");
    std::vector<FlowPersona> personas;
    {
        BuildFlowCastArgs cast_args;
        cast_args.count = roster_persona_count(static_cast<int>(cfg.flow.personaCount),
                                               FlowModelRoster{cfg.flow.roster.facilitator.empty() ? std::nullopt
                                                                                                   : std::optional<std::string>(cfg.flow.roster.facilitator),
                                                               cfg.flow.roster.discussants});
        cast_args.defaultModel = cfg.llm.model.value_or("claude-haiku-4-5-20251001");
        cast_args.isLocal = is_local;
        cast_args.rng = rng;
        if (options.personaIds) cast_args.personaIds = *options.personaIds;
        cast_args.roster = FlowModelRoster{cfg.flow.roster.facilitator.empty() ? std::nullopt
                                                                               : std::optional<std::string>(cfg.flow.roster.facilitator),
                                           cfg.flow.roster.discussants};
        cast_args.warn = warn;
        auto cast = build_flow_cast(cast_args);
        if (options.possess && !(options.personaIds && !options.personaIds->empty())) apply_theme_possession(cast, theme, log, warn);
        // 価値軸 + 核主張の一括生成 (セッション 1 回の LLM 呼び出し) + flow_session_persona 永続。
        SetupSessionPersonasArgs sp;
        sp.theme = theme;
        sp.sessionId = session_id;
        sp.personas = std::move(cast);
        sp.flow = flow;
        if (!cfg.flow.personaSetupModel.empty()) sp.model = cfg.flow.personaSetupModel;
        sp.warn = warn;
        personas = setup_session_personas(llm, sp);
        std::vector<std::string> labels;
        for (const auto& p : personas) labels.push_back(p.name + "(" + p.role + "/" + p.stance + ")");
        log("ペルソナ " + std::to_string(personas.size()) + " 人生成: " + str::join(labels, ", "));
    }
    if (personas.empty()) throw std::runtime_error("run_flow: cast is empty");

    // synthetic opinions (機密タグ用)
    const std::vector<std::string> synthetic_opinions =
        std::find(tags.begin(), tags.end(), "機密") != tags.end() ? synthesize_opinions(mechanics) : std::vector<std::string>{};

    std::vector<FlowUtteranceRecord> all_utterances;
    std::vector<std::string> all_aufhebung;
    std::vector<VoteResult> round_evaluations;

    // 発話レコードの永続 + 通知 (persistUtterance → onUtterance の順序を全ターンで維持)。
    auto commit_utterance = [&](FlowUtteranceRecord record, std::vector<NamedUtterance>& round_utterances) {
        persist_utterance(record);
        round_utterances.push_back({record.personaName, record.text});
        all_utterances.push_back(record);
        if (state) state->set_utterances(static_cast<int>(all_utterances.size()));
        if (options.onUtterance) options.onUtterance(all_utterances.back());
    };

    // ── ラウンドループ ────────────────────────────────────────────────────────
    const auto facilitator_it = std::find_if(personas.begin(), personas.end(), [](const auto& p) { return p.role == "facilitator"; });
    const FlowPersona facilitator_persona = facilitator_it != personas.end() ? *facilitator_it : personas[0];

    // 承認済み論点 (09): ペーパーゲートで人間が確認した論点を開幕プロンプトの参考に注入する。
    std::vector<std::string> guiding_issues;
    if (options.paperOverride && options.paperOverride->issues)
        for (const auto& s : *options.paperOverride->issues) {
            std::string t = str::trim(s);
            if (!t.empty() && guiding_issues.size() < 5) guiding_issues.push_back(std::move(t));
        }

    const InvestigateResult* investigation = setup_result.investigation ? &*setup_result.investigation : nullptr;

    for (int round = 1; round <= rounds; ++round) {
        check_stop(state);
        if (state) {
            state->set_phase("round");
            state->set_position(round, rounds, 0);
        }
        log("ラウンド " + std::to_string(round) + "/" + std::to_string(rounds) + " 開始");
        std::vector<NamedUtterance> round_utterances;

        // ── [4] ファシリテーター開幕ターン (議題提示 + 前ラウンド文脈 + 承認済み論点) ──
        FacilitatorTurnArgs fa;
        fa.theme = theme;
        fa.round = round;
        fa.facilitator = facilitator_persona;
        fa.sessionId = session_id;
        fa.paperId = paper_id;
        fa.flow = flow;
        fa.previousRound = build_previous_round_context(paper.rounds, round_evaluations, all_utterances);
        fa.guidingIssues = guiding_issues;
        fa.warn = warn;
        if (auto rec = run_facilitator_turn(llm, fa)) commit_utterance(std::move(*rec), round_utterances);

        // ── [5] ターンループ (シャッフル輪番。facilitator は開幕専任で対象外) ──
        const auto turn_order = build_turn_order(personas, turns_per_round, rng);
        for (int turn = 1; turn <= turns_per_round; ++turn) {
            if (static_cast<std::size_t>(turn - 1) >= turn_order.size()) break;  // 発言者ゼロ (personaCount=1 等)
            check_stop(state);
            if (state) state->set_position(round, rounds, turn);
            PersonaTurnArgs pa;
            pa.persona = turn_order[static_cast<std::size_t>(turn - 1)];
            pa.round = round;
            pa.turn = turn;
            pa.paper = &paper;
            pa.sessionId = session_id;
            pa.paperId = paper_id;
            pa.flow = flow;
            pa.roundUtterances = round_utterances;
            pa.voiceCache = &voice_cache;
            pa.investigation = investigation;
            pa.syntheticOpinions = synthetic_opinions;
            pa.log = log;
            pa.warn = warn;
            if (auto rec = run_persona_turn(llm, pa)) commit_utterance(std::move(*rec), round_utterances);
        }

        // ── [6] 世論決定 (議論=中立投票 / 改善=機械スコア) + onVote 通知 ─────────
        check_stop(state);
        if (state) state->set_phase("vote");
        std::vector<const FlowUtteranceRecord*> candidates;
        for (const auto& u : all_utterances)
            if (is_vote_candidate(u, round)) candidates.push_back(&u);
        RoundEvalContext ec;
        ec.theme = theme;
        ec.sessionId = session_id;
        ec.paperId = paper_id;
        ec.round = round;
        ec.mechanics = mechanics;
        ec.tags = tags;
        for (const auto* u : candidates) ec.candidates.push_back({u->id, u->personaName, u->text});
        ec.llm = &llm;
        ec.warn = warn;
        const VoteResult evaluation = evaluate_round(ec);
        if (evaluation.winner) log("ラウンド " + std::to_string(round) + " 世論: " + *evaluation.winner);
        if (options.onVote) {
            try {
                VoteEvent ev;
                ev.round = round;
                ev.kind = evaluation.kind ? *evaluation.kind : "votes";
                ev.tally = evaluation.tally;
                ev.winner = evaluation.winner;
                for (const auto* u : candidates) ev.candidates.push_back({u->id, u->personaName});
                options.onVote(ev);
            } catch (const std::exception& e) {
                warn("ラウンド " + std::to_string(round) + " onVote 通知失敗: " + e.what());
            }
        }
        round_evaluations.push_back(evaluation);

        // ── [7] ラウンドサマリ + 止揚 + ペーパーライブ更新 ─────────────────────
        check_stop(state);
        if (state) state->set_phase("summary");
        RoundSummaryInput si;
        si.theme = theme;
        si.sessionId = session_id;
        si.paperId = paper_id;
        si.round = round;
        si.utterances = round_utterances;
        si.stockedAufhebung = all_aufhebung;
        si.warn = warn;
        si.flow = flow;
        const auto summary_result = summarize_round(llm, si);
        all_aufhebung.insert(all_aufhebung.end(), summary_result.newAufhebung.begin(), summary_result.newAufhebung.end());
        paper.rounds.push_back({round, summary_result.summary, summary_result.newAufhebung});
        // ペーパー更新 (ライブ): base ブリーフ + ここまでの議論の経過を body_md に上書き。
        try {
            update_paper_body(paper_id, render_progress_markdown(paper.bodyMd.value_or(""), paper.rounds));
        } catch (const std::exception& e) {
            warn(std::string("ペーパー更新失敗 (議論は続行): ") + e.what());
        }
        log("ラウンド " + std::to_string(round) + " 終了 (" + std::to_string(round_utterances.size()) + " 発話, 止揚: " +
            std::to_string(summary_result.newAufhebung.size()) + ")");

        // ── [9] 早期収束チェック: 止揚到達 AND 投票集中度 (respec 04) ────────────
        if (should_converge_early(all_aufhebung.size(), evaluation, log)) break;
    }

    // ── [9] 結論生成 (全ラウンドサマリを入力に含める, respec 04) ────────────────
    check_stop(state);
    if (state) state->set_phase("conclusion");
    log("発話時 RAG: lookup " + std::to_string(voice_cache.misses()) + " 回 / キャッシュ再利用 " + std::to_string(voice_cache.hits()) +
        " 回 (item5)");
    log("結論生成中...");
    GenerateConclusionArgs ca;
    ca.theme = theme;
    ca.sessionId = session_id;
    ca.paperId = paper_id;
    ca.allUtterances = all_utterances;
    ca.allAufhebung = all_aufhebung;
    ca.rounds = paper.rounds;
    ca.voteResults = round_evaluations;
    ca.warn = warn;
    ca.flow = flow;
    // 議論適性: 低のまま強行した議論は、結論にその旨を併記する (09)。
    if (options.paperOverride && options.paperOverride->debatability && !options.paperOverride->debatability->debatable)
        ca.annotation = "議論適性: 低 (両論武装可能な争点 " + std::to_string(options.paperOverride->debatability->armableBothCount) + " 件)";
    const ConclusionResult conclusion = generate_conclusion(llm, ca);
    log("結論: " + (conclusion.concluded ? str::js_slice(conclusion.summary, 0, 80) : std::string("結論なし")));

    // ── ペーパー本文の確定 (LLM リファイン opt-in + 最終焼き込み) ───────────────
    if (state) state->set_phase("finalize");
    std::string final_base = paper.bodyMd.value_or("");
    if (cfg.flow.paperRefine.enabled) {
        try {
            RefinePaperBriefInput ri;
            ri.baseBodyMd = strip_progress(final_base);
            ri.theme = theme;
            ri.rounds = paper.rounds;
            ri.conclusion = conclusion.summary;
            ri.sessionId = session_id;
            ri.flow = flow;
            if (!cfg.flow.paperRefine.model.empty()) ri.model = cfg.flow.paperRefine.model;
            ri.warn = warn;
            const auto refined = refine_paper_brief(llm, ri);
            if (refined && !str::trim(*refined).empty()) {
                final_base = *refined;
                // 構造化列 (観点補足 / メカニクス) を派生し直して追従させる (theme/tags は据え置き)。
                PaperContent fallback;
                fallback.theme = theme;
                fallback.tags = tags;
                fallback.supplement = supplement;
                fallback.mechanics = mechanics;
                const auto derived = markdown_to_paper_draft(*refined, fallback);
                update_paper_derived(paper_id, derived.supplement, derived.mechanics);
                log("ペーパーリファイン完了 (メカニクス " + std::to_string(derived.mechanics.size()) + " 件)");
            }
        } catch (const std::exception& e) {
            warn(std::string("ペーパーリファインスキップ (議論は続行): ") + e.what());
        }
    }
    // ペーパー最終更新 (ライブ): 議論の経過 + 結論まで焼き込む (リファイン済みなら refined base)。
    try {
        update_paper_body(paper_id, render_progress_markdown(final_base, paper.rounds, conclusion.summary));
    } catch (const std::exception& e) {
        warn(std::string("ペーパー最終更新失敗: ") + e.what());
    }
    if (state) state->set_phase("done");

    FlowDirectorResult out;
    out.sessionId = session_id;
    out.paperId = paper_id;
    out.utterances = std::move(all_utterances);
    out.rounds = static_cast<int>(paper.rounds.size());
    out.conclusion = conclusion.summary;
    out.concluded = conclusion.concluded;
    return out;
}

FlowDirectorResult run_discussion_flow(persona_engine::LLMClient& llm, const std::string& theme, const std::vector<FlowTag>& tags,
                                       const FlowDirectorDeps& deps) {
    FlowRunOptions o;
    static_cast<FlowDirectorDeps&>(o) = deps;
    o.flow = "discussion";
    return run_flow(llm, theme, tags, o);
}

} // namespace di::flow
