#include "flow/dialectic/driver.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "flow/argument_graph.hpp"
#include "flow/cost_logger.hpp"
#include "flow/dialectic/conclusion.hpp"
#include "flow/dialectic/convergence.hpp"
#include "flow/dialectic/exchange.hpp"
#include "flow/dialectic/exchange_runner.hpp"
#include "flow/dialectic/goal.hpp"
#include "flow/dialectic/issue_voices.hpp"
#include "flow/dialectic/issues.hpp"
#include "flow/dialectic/outcome.hpp"
#include "flow/dialectic/position.hpp"
#include "flow/dialectic/position_stance.hpp"
#include "flow/dialectic/scheduler.hpp"
#include "flow/dialectic/store.hpp"
#include "flow/dialectic/synthesis.hpp"
#include "flow/dialectic/tension.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/flow_setup.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/persona_cast.hpp"
#include "flow/persona_setup.hpp"
#include "flow/proxy/position_attribution.hpp"
#include "flow/proxy/stance.hpp"
#include "flow/voice_cache.hpp"
#include "flow/vote.hpp"

namespace di::flow::dialectic {

namespace {

bool is_debater_side(const FlowPersona& p) { return p.role == "debater" && (p.stance == "pro" || p.stance == "con"); }

void check_stop(const std::shared_ptr<FlowRunState>& state) {
    if (state && state->stop_requested()) {
        state->set_phase("stopped");
        throw FlowStopped("flow stopped by request");
    }
}

std::optional<std::string> non_empty(const std::string& s) {
    if (s.empty()) return std::nullopt;
    return s;
}

std::unique_ptr<persona_engine::LLMClient> logged(persona_engine::LLMClient& llm, CostLogContext ctx) {
    return std::make_unique<CostLoggedClient>(llm, std::move(ctx));
}

CostLogContext ctx_of(const std::string& flow, const std::string& session_id, const std::string& location) {
    CostLogContext c;
    c.flow = flow;
    c.sessionId = session_id;
    c.location = location;
    return c;
}

} // namespace

EnsuredDebaters ensure_debaters(const std::vector<FlowPersona>& cast, const WarnFn& warn) {
    std::vector<FlowPersona> working = cast;
    auto find_debater = [&](const std::string& stance) -> std::optional<FlowPersona> {
        for (const auto& p : working)
            if (p.role == "debater" && p.stance == stance) return p;
        return std::nullopt;
    };
    std::optional<FlowPersona> pro = find_debater("pro");
    std::optional<FlowPersona> con = find_debater("con");
    if (pro && con) return {*pro, *con, working};

    for (const std::string stance : {"pro", "con"}) {
        if (stance == "pro" ? pro.has_value() : con.has_value()) continue;
        auto it = std::find_if(working.begin(), working.end(), [&](const FlowPersona& p) {
            return p.role != "facilitator" && !(pro && p.id == pro->id) && !(con && p.id == con->id) && !is_debater_side(p);
        });
        if (it == working.end())
            throw std::runtime_error("dialectic エンジンには pro/con の debater が 2 人必要です (flow.personaCount を 4 以上にしてください)");
        FlowPersona promoted = *it;
        promoted.role = "debater";
        promoted.stance = stance;
        *it = promoted;
        if (warn) warn("debater 不足: 「" + promoted.name + "」を " + stance + " 側 debater に昇格 (degrade)");
        if (stance == "pro")
            pro = promoted;
        else
            con = promoted;
    }
    return {*pro, *con, working};
}

FlowDirectorResult run_dialectic_flow(persona_engine::LLMClient& llm, const std::string& theme, const std::vector<FlowTag>& tags,
                                      const FlowRunOptions& options) {
    const auto& cfg = config::get_config();
    const ListExternalVoicesFn list_external_voices =
        options.listExternalVoices ? options.listExternalVoices
                                   : ListExternalVoicesFn([](const std::vector<std::string>&, int) { return std::vector<ContextVoice>{}; });
    const WarnFn log = options.log ? options.log : WarnFn([](const std::string& m) { di::log::info("[flow/dialectic] " + m); });
    const WarnFn warn = options.warn ? options.warn : WarnFn([](const std::string& m) { di::log::warn("[flow/dialectic/warn] " + m); });
    const Rng rng = options.rng ? options.rng : Rng([] { return crypto::random_unit(); });
    const std::string& flow = options.flow;
    const auto& state = options.state;

    const std::string session_id = options.sessionId ? *options.sessionId : crypto::uuid_v4();
    const bool is_local = cfg.llm.backend == "local";
    const std::optional<std::string> judge_model = non_empty(cfg.flow.dialectic.judgeModel);
    const std::optional<std::string> final_judge_model =
        cfg.flow.dialectic.finalJudgeModel.empty() ? judge_model : std::optional<std::string>(cfg.flow.dialectic.finalJudgeModel);
    VoiceCache voice_cache(list_external_voices);

    // ── ペーパー初期化 (既存 setup_flow_paper 流用: investigate 省略/増補/persist_paper 込み) ──
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
    // setup_flow_paper は全経路で bodyMd を持たせる。system 固定 (プロンプトキャッシュ戦略の維持)。
    const std::string paper_system = paper.bodyMd.value_or("");
    check_stop(state);

    // ── キャスト (stance セッション固定 + 価値軸/核主張 + flow_session_persona 永続) ──
    // 代理モード: 進行役 1 人 + 代理 2 人。代理には価値軸/核主張を生成しない (本人の発言に無い主張を作らない)。
    if (state) state->set_phase("cast");
    const std::vector<FlowPersona> proxy_participants = options.proxyParticipants.value_or(std::vector<FlowPersona>{});
    const bool proxy_mode = !proxy_participants.empty();
    if (proxy_mode && proxy_participants.size() != 2)
        throw std::runtime_error("代理議論の参加者は 2 人です (指定 " + std::to_string(proxy_participants.size()) + " 人)");
    const FlowModelRoster roster{cfg.flow.roster.facilitator.empty() ? std::nullopt : std::optional<std::string>(cfg.flow.roster.facilitator),
                                 cfg.flow.roster.discussants};
    BuildFlowCastArgs cast_args;
    cast_args.count = proxy_mode ? 1 : roster_persona_count(static_cast<int>(cfg.flow.personaCount), roster);
    cast_args.defaultModel = cfg.llm.model.value_or("claude-haiku-4-5-20251001");
    cast_args.isLocal = is_local;
    cast_args.rng = rng;
    if (!proxy_mode && options.personaIds) cast_args.personaIds = *options.personaIds;
    cast_args.roster = roster;
    cast_args.warn = warn;
    const auto seed_cast = build_flow_cast(cast_args);
    if (seed_cast.empty()) throw std::runtime_error("run_dialectic_flow: cast is empty");
    SetupSessionPersonasArgs sp;
    sp.theme = theme;
    sp.sessionId = session_id;
    sp.personas = proxy_mode ? std::vector<FlowPersona>{seed_cast[0]} : seed_cast;
    sp.flow = flow;
    if (!cfg.flow.personaSetupModel.empty()) sp.model = cfg.flow.personaSetupModel;
    sp.warn = warn;
    const auto setup_personas = setup_session_personas(llm, sp);
    if (setup_personas.empty()) throw std::runtime_error("run_dialectic_flow: cast is empty");
    if (proxy_mode) persist_session_personas(session_id, proxy_participants);
    std::vector<FlowPersona> enriched;
    if (proxy_mode) {
        enriched.push_back(setup_personas[0]);
        enriched.insert(enriched.end(), proxy_participants.begin(), proxy_participants.end());
    } else {
        enriched = setup_personas;
    }
    EnsuredDebaters initial = proxy_mode ? EnsuredDebaters{enriched[1], enriched[2], enriched} : ensure_debaters(enriched, warn);
    FlowPersona pro = initial.pro;
    FlowPersona con = initial.con;
    const std::vector<FlowPersona> cast = initial.cast;
    const auto fac_it = std::find_if(cast.begin(), cast.end(), [](const FlowPersona& p) { return p.role == "facilitator"; });
    const FlowPersona facilitator = fac_it != cast.end() ? *fac_it : cast[0];
    std::vector<FlowPersona> speakers;
    for (const auto& p : cast)
        if (p.role != "facilitator") speakers.push_back(p);
    std::map<std::string, std::string> persona_names;
    for (const auto& p : cast) persona_names.insert_or_assign(p.id, p.name);
    {
        std::vector<std::string> labels;
        for (const auto& p : cast) labels.push_back(p.name + "(" + p.role + "/" + p.stance + ")");
        log("ペルソナ " + std::to_string(cast.size()) + " 人: " + str::join(labels, ", ") + " / engine=dialectic");
    }

    // ── セッション状態 (決定的コードが持つ) ──────────────────────────────────
    ArgumentGraph graph;
    std::vector<FlowUtteranceRecord> all_utterances;
    std::map<std::string, UtteranceDigest> utterance_index;
    std::map<std::string, int> speak_counts;
    std::vector<VoteResult> vote_results;
    std::vector<IssueSettlement> settlements;
    std::vector<std::string> ratified_aufheben;
    double global_turn = 0;

    /** 発話の永続 + 通知 + インデックス更新 (persist_utterance → onUtterance の順序維持)。 */
    auto commit = [&](const FlowUtteranceRecord& record) {
        persist_utterance(record);
        all_utterances.push_back(record);
        utterance_index.insert_or_assign(record.id, UtteranceDigest{record.id, record.personaName, record.text});
        if (record.role != "facilitator" && !record.isError) speak_counts[record.personaId] += 1;
        if (state) {
            state->set_utterances(static_cast<int>(all_utterances.size()));
        }
        if (options.onUtterance) options.onUtterance(all_utterances.back());
        check_stop(state);
    };

    auto make_record = [&](const RecordArgs& a) {
        FlowUtteranceRecord u;
        u.id = crypto::uuid_v4();
        u.sessionId = session_id;
        u.paperId = paper_id;
        u.round = a.round;
        u.turn = a.turn;
        u.personaId = a.persona.id;
        u.personaName = a.persona.name;
        u.role = a.persona.role;
        u.stance = persona_stance(a.persona);
        u.text = a.text;
        u.isError = a.isError;
        if (a.persona.possession) u.possessionName = a.persona.possession->label;
        if (a.act && !a.act->empty()) u.act = a.act;
        if (a.targetId && !a.targetId->empty()) u.targetId = a.targetId;
        if (a.claimKind && !a.claimKind->empty()) u.claimKind = a.claimKind;
        if (a.evidenceIds && !a.evidenceIds->empty()) u.evidenceIds = a.evidenceIds;
        return u;
    };
    auto simple = [](const FlowPersona& persona, int round, int turn, const std::string& text) {
        RecordArgs a;
        a.persona = persona;
        a.round = round;
        a.turn = turn;
        a.text = text;
        return a;
    };

    CostLogContext fac_ctx = ctx_of(flow, session_id, "facilitator");
    fac_ctx.role = "facilitator";
    fac_ctx.persona = facilitator.name;
    CostLoggedClient facilitator_llm(llm, fac_ctx);

    // ── [0] 論点分解 (paperOverride.issues があれば再分解しない — PR-D 共用) ────
    const auto& override_paper = options.paperOverride;
    std::vector<std::string> issue_titles;
    std::string issue_source;
    std::vector<std::string> approved;
    if (override_paper && override_paper->issues)
        for (const auto& s : *override_paper->issues) {
            std::string t = str::trim(s);
            if (!t.empty()) approved.push_back(std::move(t));
        }
    if (!approved.empty()) {
        if (approved.size() > static_cast<std::size_t>(ISSUES_MAX)) approved.resize(static_cast<std::size_t>(ISSUES_MAX));
        issue_titles = approved;
        issue_source = "paper-gate";
        log("[0] 承認済み論点 " + std::to_string(issue_titles.size()) + " 件を採用 (再分解しない)");
    } else {
        issue_source = "llm";
        try {
            CostLoggedClient decompose_llm(llm, ctx_of(flow, session_id, "issue-decompose"));
            DecomposeIssuesArgs da;
            da.theme = theme;
            da.paperMd = paper_system;
            da.model = facilitator.model;
            da.warn = warn;
            issue_titles = decompose_issues(decompose_llm, da);
        } catch (const std::exception& e) {
            warn(std::string("[0] 論点分解失敗: ") + e.what() + " — テーマ 1 論点で degrade");
            issue_titles.clear();
        }
        if (issue_titles.empty()) {
            warn("[0] 論点 0 件 — テーマそのものを単一論点として続行 (degrade)");
            issue_titles = {theme};
        }
        log("[0] 論点分解: " + std::to_string(issue_titles.size()) + " 件");
    }

    std::vector<IssueRecord> issues;
    for (std::size_t i = 0; i < issue_titles.size(); ++i) {
        IssueRecord rec;
        rec.sessionId = session_id;
        rec.title = issue_titles[i];
        rec.ordinal = static_cast<int>(i + 1);
        rec.source = issue_source;
        rec.status = "open";
        issues.push_back(insert_issue(rec));
    }
    const int issue_count = static_cast<int>(issues.size());
    if (state) state->set_position(0, issue_count, 0);

    // 代理モード: 最初に「論者は AI の代理で、本人の発言・同意ではない」と伝える (どの論点から始まっても必ず出す)。
    if (proxy_mode) {
        std::vector<std::string> names;
        for (const auto& p : proxy_participants) names.push_back(p.name);
        const std::string notice = "この議論の論者 (" + str::join(names, "・") + ") は、公開レビューの発言から作った AI の代理です。" +
                                   "本人の発言・同意ではありません。本人の発言に基づく発言には [E番号]、推測には（推測）を付けます。";
        commit(make_record(simple(facilitator, 1, 0, notice)));
    }

    auto update_paper_live = [&](const std::string& fail_prefix, const std::optional<std::string>& conclusion) {
        try {
            update_paper_body(paper_id, render_progress_markdown(paper.bodyMd.value_or(""), paper.rounds, conclusion));
        } catch (const std::exception& e) {
            warn(fail_prefix + e.what());
        }
    };

    // ── issue ループ ([1]〜[5] は issue ごとに独立に回る) ─────────────────────
    bool converged_early = false;
    for (auto& issue : issues) {
        if (converged_early) break;  // 収束後の issue は open のまま結論に「未討議」で残る
        check_stop(state);
        const int round = issue.ordinal;
        const std::string rs = std::to_string(round);
        if (state) {
            state->set_phase("round");
            state->set_position(round, issue_count, 0);
        }
        log("論点 " + rs + "/" + std::to_string(issue_count) + ": " + issue.title);

        // 代理モード: 論点ごとに本人のデータから賛否を決める。対立しなければ反対役を作らずに結論なし。
        if (proxy_mode) {
            const std::vector<FlowPersona> proxies = {enriched[1], enriched[2]};
            CostLogContext sc = ctx_of(flow, session_id, "proxy-stance");
            sc.round = round;
            CostLoggedClient stance_llm(llm, sc);
            std::vector<proxy::ProxyStance> stances;
            for (const auto& p : proxies) {
                proxy::JudgeProxyStanceArgs ja;
                ja.issueTitle = issue.title;
                ja.bundleText = p.proxy ? p.proxy->bundleText : std::string();
                ja.llm = &stance_llm;
                ja.model = cfg.flow.proxy.stanceModel.empty() ? judge_model : std::optional<std::string>(cfg.flow.proxy.stanceModel);
                ja.warn = warn;
                stances.push_back(proxy::judge_proxy_stance(ja));
            }
            const auto sides = proxy::assign_issue_sides(stances, {proxies[0].name, proxies[1].name});
            log("論点 " + rs + ": 代理の立場 " + proxies[0].name + "=" + stances[0] + ", " + proxies[1].name + "=" + stances[1]);
            if (!sides.opposed) {
                commit(make_record(simple(facilitator, round, 0, render_outcome_line("none", sides.reason))));
                update_issue_status(issue.id, "concluded");
                issue.status = "concluded";
                IssueSettlement settlement;
                settlement.issue = issue;
                settlement.undecided = sides.reason;
                settlements.push_back(settlement);
                VoteResult empty;
                empty.kind = "votes";
                vote_results.push_back(empty);
                paper.rounds.push_back(settlement_to_round_summary(settlement, persona_names, std::nullopt));
                update_paper_live("ペーパー更新失敗 (議論は続行): ", std::nullopt);
                continue;
            }
            pro = proxies[static_cast<std::size_t>(sides.pro)];
            pro.stance = "pro";
            con = proxies[static_cast<std::size_t>(sides.con)];
            con.stance = "con";
            speakers = {pro, con};
        }

        // ファシリテーター開幕 (露出文のレンダリングのみ LLM)。
        const std::string opening_text = render_facilitator_line(
            facilitator_llm,
            "論点 " + rs + "「" + issue.title + "」について議論します。まず賛成側の" + pro.name + "さん、続いて反対側の" + con.name +
                "さんに定立をお願いします。",
            facilitator.model, warn);
        commit(make_record(simple(facilitator, round, 0, opening_text)));

        // 論点ごとのプレイヤーの声 (集めた外部の声)。定立と反論のプロンプトに根拠のデータとして載せる。
        const auto issue_voices = voice_cache.lookup({issue.title, theme}, static_cast<int>(cfg.flow.paperRichness.voices));
        const std::string voices_block = render_issue_voices(issue_voices);
        log("論点 " + rs + ": プレイヤーの声 " + std::to_string(issue_voices.size()) + " 件を議論者に渡す");

        // ── [1] 定立 (主張 → 論拠 1〜3 件を陣営交互に 1 件ずつ投げ合う) ────────────
        std::vector<PositionRecord> positions;
        positions.reserve(2);  // positionBySide が要素を指すので再確保させない
        PositionsBySide position_by_side;
        struct Opening {
            FlowPersona persona;
            std::string stance;
            std::size_t position;
            GeneratedPosition gp;
            std::string claimId;
        };
        std::vector<Opening> openings;
        GroundExchange exchange(ExchangeOptions{std::max(1, static_cast<int>(cfg.flow.dialectic.argumentDepth))});
        int turn = 0;
        for (const FlowPersona* persona_ptr : {&pro, &con}) {
            const FlowPersona& persona = *persona_ptr;
            turn += 1;
            if (state) state->set_position(round, issue_count, turn);
            const std::string stance = persona.stance == "con" ? "con" : "pro";
            CostLogContext pc = ctx_of(flow, session_id, "position");
            pc.round = round;
            pc.turn = turn;
            pc.role = persona.role;
            pc.persona = persona.name;
            CostLoggedClient position_llm(llm, pc);
            GeneratePositionArgs ga;
            ga.theme = theme;
            ga.issue = issue;
            ga.persona = persona;
            ga.stance = stance;
            ga.paperSystem = paper_system;
            ga.voicesBlock = voices_block;
            ga.model = persona.model;
            ga.warn = warn;
            GeneratedPosition gp = generate_position(position_llm, ga);
            // 定立が陣営と逆向き (賛成なのに否定 / 反対なのに肯定) なら 1 回だけ作り直す (neco レビュー 2026-10-13)。
            if (cfg.flow.dialectic.ruleCheck && !gp.degraded) {
                CostLogContext stc = ctx_of(flow, session_id, "position-stance");
                stc.round = round;
                stc.turn = turn;
                CostLoggedClient stance_llm(llm, stc);
                JudgePositionStanceArgs ja;
                ja.issue = issue;
                ja.claim = gp.claim;
                ja.qualifier = gp.qualifier;
                ja.model = judge_model;
                ja.warn = warn;
                const auto answer = judge_position_stance(stance_llm, ja);
                if (is_reversed(stance, answer)) {
                    warn("論点 " + rs + " (" + persona.name + "/" + stance + "): 定立が立場と逆向き (" + answer + ") — 作り直す");
                    GeneratePositionArgs again = ga;
                    again.feedback = stance_feedback(stance, gp.claim);
                    gp = generate_position(position_llm, again);
                }
            }
            // 代理: 本人の発言と申告した論拠を検査し、通らなければ推測に落とす (定立は捨てない)。
            gp = proxy::attribute_position_grounds(gp, persona, warn);
            PositionRecord pr;
            pr.issueId = issue.id;
            pr.personaId = persona.id;
            pr.stance = stance;
            pr.claim = gp.claim;
            pr.qualifier = gp.qualifier;
            pr.grounds = gp.grounds;
            pr.values = gp.values;
            positions.push_back(insert_position(pr));
            position_by_side.set(stance, &positions.back());
            RecordArgs ra = simple(persona, round, turn, gp.text);
            ra.act = "claim";
            const auto record = make_record(ra);
            commit(record);
            graph.apply(GraphUtterance{record.id, persona.id, "claim", std::nullopt, ++global_turn});
            openings.push_back(Opening{persona, stance, positions.size() - 1, gp, record.id});
        }
        std::size_t ground_rounds = 0;
        for (const auto& o : openings) ground_rounds = std::max(ground_rounds, positions[o.position].grounds.size());
        for (std::size_t i = 0; i < ground_rounds; ++i) {
            for (const auto& o : openings) {
                const auto& grounds = positions[o.position].grounds;
                if (i >= grounds.size()) continue;
                const Ground ground = grounds[i];
                const std::string say = i < o.gp.groundTexts.size() ? o.gp.groundTexts[i] : ground.text;
                // degrade で主張と同文になった論拠は再投稿せず、主張の発話を論拠の投稿として扱う。
                std::string utterance_id = o.claimId;
                if (say != o.gp.text) {
                    turn += 1;
                    if (state) state->set_position(round, issue_count, turn);
                    // 論拠は自分の主張の下に並べるだけで、誰かへの応答ではない (宛先を付けない)。
                    RecordArgs ra = simple(o.persona, round, turn, say);
                    ra.act = "claim";
                    if (ground.claimKind && !ground.claimKind->empty()) {
                        ra.claimKind = ground.claimKind;
                        ra.evidenceIds = ground.evidenceIds.value_or(std::vector<std::string>{});
                    }
                    const auto record = make_record(ra);
                    commit(record);
                    graph.apply(GraphUtterance{record.id, o.persona.id, "claim", std::nullopt, ++global_turn});
                    utterance_id = record.id;
                }
                NewNode nn;
                nn.utteranceId = utterance_id;
                nn.side = o.stance;
                nn.personaId = o.persona.id;
                nn.groundId = ground.id;
                nn.data = ground.text;
                if (ground.warrant && !ground.warrant->empty()) nn.warrant = ground.warrant;
                if (ground.kind) nn.kind = ground.kind;
                nn.text = say;
                exchange.add_opening(nn);
            }
        }

        // ── [2] 論拠の応酬 (相手の論拠に「反論 = 論拠 1 件」か「認める」で答える → 落ち着いたらまとめ) ──
        {
            GroundExchangeArgs ea;
            ea.issue = issue;
            ea.round = round;
            ea.turn = turn;
            ea.exchange = &exchange;
            ea.positionBySide = &position_by_side;
            ea.speakers = speakers;
            ea.debaters = Debaters{pro, con};
            ea.facilitator = facilitator;
            ea.personaNames = persona_names;
            ea.utteranceIndex = &utterance_index;
            ea.speakCounts = &speak_counts;
            ea.paperSystem = paper_system;
            ea.voicesBlock = voices_block;
            ea.turnCap = std::max(0, static_cast<int>(cfg.flow.dialectic.exchangeTurns));
            ea.ruleCheck = cfg.flow.dialectic.ruleCheck;
            ea.judgeModel = judge_model;
            ea.turnLlm = [&](const FlowPersona& persona, int t) {
                if (state) state->set_position(round, issue_count, t);
                CostLogContext c = ctx_of(flow, session_id, "utterance");
                c.round = round;
                c.turn = t;
                c.role = persona.role;
                c.persona = persona.name;
                return logged(llm, c);
            };
            ea.ruleCheckLlm = [&](int t) {
                CostLogContext c = ctx_of(flow, session_id, "rule-check");
                c.round = round;
                c.turn = t;
                return logged(llm, c);
            };
            ea.facilitatorLlm = &facilitator_llm;
            ea.makeRecord = make_record;
            ea.commit = commit;
            ea.applyGraph = [&](const FlowUtteranceRecord& record, const DialogueAct& act, const std::optional<std::string>& target_id) {
                graph.apply(GraphUtterance{record.id, record.personaId, act, target_id, ++global_turn});
            };
            ea.log = log;
            ea.warn = warn;
            turn = run_ground_exchange(ea);
        }

        // ── [3] 矛盾の特定 (型分類 + 事実解消) ─────────────────────────────────
        PositionRecord& position_a = positions[0];
        PositionRecord& position_b = positions[1];
        std::vector<TensionRecord> tensions;
        std::vector<SynthesisRecord> syntheses;
        // ── [4] 結論は 止揚 / 論破 / 結論なし のいずれか (dialectic.md §4.5)。
        //    論破を先に判定 (一方の根拠がすべて崩れた = 意見が出なくなった。コードのみ)。
        //    譲歩で崩れても、反論に応答できずに崩れても論破として決着する。
        const RefutationResult refutation = detect_refutation(position_a, position_b);

        if (refutation.note && !refutation.note->empty()) {
            log("論点 " + rs + ": 論破で決着");
            commit(make_record(simple(facilitator, round, ++turn, render_outcome_line("refuted", *refutation.note))));
        } else if (refutation.undecided && !refutation.undecided->empty()) {
            // 双方の論拠がすべて崩れた = どちらの主張も今ある材料では支えられない。勝敗は付けず結論なし。
            log("論点 " + rs + ": 双方の論拠が崩れて結論なし");
            commit(make_record(simple(facilitator, round, ++turn, render_outcome_line("none", *refutation.undecided))));
        } else {
            CostLogContext cc = ctx_of(flow, session_id, "tension-classify");
            cc.round = round;
            CostLoggedClient classify_llm(llm, cc);
            ClassifyTensionArgs ca;
            ca.issue = issue;
            ca.positionA = position_a;
            ca.positionB = position_b;
            ca.model = judge_model;
            ca.warn = warn;
            const TensionType type = classify_tension(classify_llm, ca);
            TensionRecord tr;
            tr.issueId = issue.id;
            tr.positionA = position_a.id;
            tr.positionB = position_b.id;
            tr.type = type;
            tr.status = "open";
            tensions.push_back(insert_tension(tr));
            TensionRecord& tension = tensions.back();
            log("論点 " + rs + " の対立型: " + type);

            if (type == "facts") {
                // 事実対立は弁証法のスコープ外: RAG 照会 → 判定。照会の結果にかかわらず論点の結論は「結論なし」。
                CostLogContext fc = ctx_of(flow, session_id, "fact-resolve");
                fc.round = round;
                CostLoggedClient fact_llm(llm, fc);
                ResolveFactArgs fa;
                fa.theme = theme;
                fa.issue = issue;
                fa.positionA = position_a;
                fa.positionB = position_b;
                fa.listExternalVoices = list_external_voices;
                fa.model = judge_model;
                fa.warn = warn;
                const auto resolution = resolve_fact_tension(fact_llm, fa);
                tension.status = resolution.status;
                tension.resolutionNote = resolution.note;
                update_tension_status(tension.id, resolution.status, resolution.note);
                const std::string fact_text =
                    resolution.status == "resolved"
                        ? "事実の対立は照会で確かめました (" + resolution.note + ")。止揚にも論破にも至っていません。"
                        : std::string("事実の対立は照会でも解消できませんでした。未解決の事実問題として結論に明記します。");
                commit(make_record(simple(facilitator, round, ++turn, render_outcome_line("none", fact_text))));
            } else {
                // ── 止揚 (生成 → 折衷ゲート → 敵対的批准 → 修正ループ ≤2)。
                //    批准されなければ (折衷・批准不成立) 結論なしとして理由を残す。
                auto round_ctx = [&](const std::string& location) {
                    CostLogContext c = ctx_of(flow, session_id, location);
                    c.round = round;
                    return c;
                };
                CostLoggedClient gen_llm(llm, round_ctx("synthesize"));
                CostLoggedClient gate_llm(llm, round_ctx("elevation-gate"));
                CostLoggedClient ratify_llm(llm, round_ctx("ratify"));
                SynthesisLoopArgs sa;
                sa.issueTitle = issue.title;
                sa.tension = tension;
                sa.positionA = position_a;
                sa.positionB = position_b;
                sa.personaNames = persona_names;
                sa.paperSystem = paper_system;
                sa.genLlm = &gen_llm;
                sa.gateLlm = &gate_llm;
                sa.ratifyLlm = &ratify_llm;
                // 生成は進行役。結論候補の評価・批准は頻繁な通常判定と分離する。
                sa.mainModel = facilitator.model;
                sa.judgeModel = final_judge_model;
                sa.warn = warn;
                const auto outcome = run_synthesis_loop(sa);
                syntheses.push_back(outcome.record);
                tension.status = outcome.tensionStatus;
                update_tension_status(tension.id, outcome.tensionStatus);
                const std::string outcome_text =
                    outcome.tensionStatus == "synthesized"
                        ? render_outcome_line("aufheben", outcome.record.text)
                        : render_outcome_line("none", no_conclusion_reason(tension, &outcome.record).value_or(outcome.record.text));
                RecordArgs ra = simple(facilitator, round, ++turn, outcome_text);
                ra.act = "synthesize";
                const auto synth_record = make_record(ra);
                commit(synth_record);
                graph.apply(GraphUtterance{synth_record.id, facilitator.id, "synthesize", std::nullopt, ++global_turn});
                // 折衷は止揚ストックに数えない (dialectic.md §4)。
                if (outcome.tensionStatus == "synthesized") ratified_aufheben.push_back(outcome.record.text);
            }
        }

        update_issue_status(issue.id, "concluded");
        issue.status = "concluded";
        IssueSettlement settlement;
        settlement.issue = issue;
        settlement.positions = positions;
        settlement.tensions = tensions;
        settlement.syntheses = syntheses;
        settlement.refutation = refutation.note;
        settlement.undecided = refutation.undecided;
        settlements.push_back(settlement);

        // ── issue 決着ごとのレンズ投票 (03 流用。winnerShare が収束シグナル (b)) ──
        check_stop(state);
        if (state) state->set_phase("vote");
        std::vector<RoundUtterance> candidates;
        for (const auto& u : all_utterances)
            if (u.round == round && !u.isError && u.role != "facilitator") candidates.push_back({u.id, u.personaName, u.text});
        VoteResult evaluation;
        if (!candidates.empty()) {
            RunRoundVoteArgs va;
            va.theme = theme;
            va.sessionId = session_id;
            va.round = round;
            va.voterCount = static_cast<int>(cfg.flow.voterCount);
            va.utterances = candidates;
            va.lenses = cfg.flow.voteLenses;
            va.rng = rng;
            va.warn = warn;
            evaluation = run_round_vote(llm, va);
        } else {
            evaluation.kind = "votes";
        }
        vote_results.push_back(evaluation);
        if (options.onVote) {
            try {
                VoteEvent ev;
                ev.round = round;
                ev.kind = evaluation.kind.value_or("votes");
                ev.tally = evaluation.tally;
                ev.winner = evaluation.winner;
                for (const auto& c : candidates) ev.candidates.push_back({c.id, c.personaName});
                options.onVote(ev);
            } catch (const std::exception& e) {
                warn("論点 " + rs + " onVote 通知失敗: " + e.what());
            }
        }

        // ── ペーパーライブ更新 (issue 決着表を「議論の経過」として焼き直す) ─────
        paper.rounds.push_back(settlement_to_round_summary(settlement, persona_names, evaluation.winnerShare));
        update_paper_live("ペーパー更新失敗 (議論は続行): ", std::nullopt);

        // ── [5] 計測型収束 (コードのみ・2/3 + aufhebungTarget 上限キャップ) ──────
        ConvergenceInput ci;
        ci.unresolvedRebuttals = static_cast<double>(graph.unresolved_rebuttals().size());
        ci.winnerShare = evaluation.winnerShare;
        ci.convergeShare = cfg.flow.convergeShare;
        ci.newClaimsInWindow = graph.new_claims_since(global_turn - std::max(1.0, cfg.flow.dialectic.staleTurns));
        ci.ratifiedAufhebenCount = static_cast<double>(ratified_aufheben.size());
        ci.aufhebungTarget = cfg.facilitator.aufhebungTarget;
        const auto verdict = assess_convergence(ci);
        log("収束判定: " + verdict.note);
        if (verdict.converged && settlements.size() < issues.size()) {
            if (issue_source == "paper-gate") {
                // 人が指定・承認した論点は「それぞれ議論する」指示なので、収束シグナルが立っても打ち切らない。
                log("計測型収束シグナル " + std::to_string(verdict.met) + "/3 — 人が指定した論点のため打ち切らずに続ける");
            } else {
                log("計測型収束シグナル " + std::to_string(verdict.met) + "/3" + (verdict.capReached ? " (上限キャップ)" : "") +
                    " — 残り論点を打ち切り");
                converged_early = true;
            }
        }
    }

    // ── 結論 (issue 決着表 + 未解決の事実問題 + 投票集中度) ────────────────────
    check_stop(state);
    if (state) state->set_phase("conclusion");
    log("結論生成中...");
    ConcludeDialecticArgs cda;
    cda.theme = theme;
    cda.sessionId = session_id;
    cda.paperId = paper_id;
    cda.flow = flow;
    cda.settlements = settlements;
    cda.personaNames = persona_names;
    cda.allUtterances = all_utterances;
    cda.voteResults = vote_results;
    cda.ratifiedAufheben = ratified_aufheben;
    if (override_paper && override_paper->debatability && !override_paper->debatability->debatable)
        cda.debatabilityNote =
            "議論適性: 低 (両論武装可能な争点 " + std::to_string(override_paper->debatability->armableBothCount) + " 件)";
    cda.warn = warn;
    const ConclusionResult conclusion_result = conclude_dialectic(llm, cda);
    log("結論: " + (conclusion_result.concluded ? str::js_slice(conclusion_result.summary, 0, 80) : std::string("結論なし")));

    // ペーパー最終焼き込み (経過 + 結論)。
    if (state) state->set_phase("finalize");
    update_paper_live("ペーパー最終更新失敗: ", conclusion_result.summary);
    if (state) state->set_phase("done");

    FlowDirectorResult out;
    out.sessionId = session_id;
    out.paperId = paper_id;
    out.utterances = std::move(all_utterances);
    out.rounds = static_cast<int>(settlements.size());
    out.conclusion = conclusion_result.summary;
    out.concluded = conclusion_result.concluded;
    return out;
}

FlowDirectorResult run_dialectic_discussion_flow(persona_engine::LLMClient& llm, const std::string& theme,
                                                 const std::vector<FlowTag>& tags, const FlowDirectorDeps& deps) {
    FlowRunOptions o;
    static_cast<FlowDirectorDeps&>(o) = deps;
    o.flow = "discussion";
    return run_dialectic_flow(llm, theme, tags, o);
}

FlowDirectorResult run_dialectic_improvement_flow(persona_engine::LLMClient& llm, const std::string& theme,
                                                  const std::vector<FlowTag>& tags, const FlowDirectorDeps& deps) {
    FlowRunOptions o;
    static_cast<FlowDirectorDeps&>(o) = deps;
    o.flow = "improvement";
    return run_dialectic_flow(llm, theme, tags, o);
}

} // namespace di::flow::dialectic
