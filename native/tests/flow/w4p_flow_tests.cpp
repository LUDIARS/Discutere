// w4-P: dialectic ドライバ・4 フローの振り分け (dispatch)・改善・壁打ち・未来予測 (camps / phase / after-flow)・投票表示のテスト。
//   - golden: Node と同じ偽の LLM 応答で議論を最後まで回し、LLM への呼び出し列・DB に残る全行・ペーパー md・
//     通知・log/warn・戻り値が一致すること (tests/golden/w4-P-flow.gen.ts)。
//     tests/flow/dialectic-driver.test.ts / proxy-debate.test.ts の代理 E2E / improvement.test.ts / effect-predict.test.ts の
//     改善統合 / vote-lenses.test.ts の改善統合 / sparring.test.ts / foresight.test.ts のフェーズの各シナリオを含む。
//   - 結合: dispatch の振り分けと未来予測の後段 (entrypoints.test.ts / foresight.test.ts の注入ドライバ部分)。
//   - 進行状態 (C++ 版の追加)・壁打ちの並行呼び出し・境界 (上限ちょうど / +1 / 壊れた / 巨大な入力)。
#include "check.hpp"
#include "flow/w3l_support.hpp"

#include <atomic>
#include <filesystem>
#include <thread>

#include "base/sqlite.hpp"
#include "base/strings.hpp"
#include "flow/db/connection.hpp"
#include "flow/design_gap.hpp"
#include "flow/dialectic/conclusion.hpp"
#include "flow/dialectic/driver.hpp"
#include "flow/dialectic/exchange_runner.hpp"
#include "flow/dispatch.hpp"
#include "flow/foresight/after_flow.hpp"
#include "flow/foresight/camps.hpp"
#include "flow/foresight/phase.hpp"
#include "flow/foresight/state.hpp"
#include "flow/improvement.hpp"
#include "flow/sentiment_vector.hpp"
#include "flow/sparring.hpp"
#include "flow/vote_display.hpp"

using namespace di;
using namespace di::flow;
using w3l::Json;

namespace {

const Json& golden() {
    static const Json doc = [] {
        auto text = di::fs::read_file(di::test::fixtures_dir() + "/flow/w4p_golden.json", 64u << 20);
        if (!text) throw di::test::Failure("fixture missing: flow/w4p_golden.json");
        return di::parse_json_or_throw(*text, {64u << 20, 5'000'000, 64});
    }();
    return doc;
}

const std::vector<std::string> kAllEnv = {
    "DISCUTERE_FLOW_ROUNDS",
    "DISCUTERE_FLOW_TURNS_PER_ROUND",
    "DISCUTERE_FLOW_PERSONA_COUNT",
    "DISCUTERE_FLOW_VOTER_COUNT",
    "DISCUTERE_FLOW_ROSTER_FACILITATOR",
    "DISCUTERE_FLOW_ROSTER_DISCUSSANTS",
    "DISCUTERE_FLOW_DIALECTIC_JUDGE_MODEL",
    "DISCUTERE_FLOW_DIALECTIC_FINAL_JUDGE_MODEL",
    "DISCUTERE_FLOW_DIALECTIC_EXCHANGE_TURNS",
    "DISCUTERE_FLOW_ENGINE",
    "DISCUTERE_FLOW_SPARRING_MAX_TURNS",
    "DISCUTERE_FLOW_FORESIGHT_ENABLED",
    "DISCUTERE_FLOW_FORESIGHT_TURNS",
    "DISCUTERE_FLOW_FORESIGHT_MODEL",
    "DISCUTERE_FLOW_FORESIGHT_FORECAST_MODEL",
    "DISCUTERE_FLOW_IMPROVEMENT_EFFECT_MODEL",
    "DISCUTERE_FACILITATOR_AUFHEBUNG_TARGET",
};

void set_env(const Json& env) {
    for (const auto& k : kAllEnv) env::set_for_test(k, std::nullopt);
    for (const auto& [k, v] : env.items()) env::set_for_test(k, std::string_view(v.get_ref<const std::string&>()));
    config::reset_config_for_test();
}

std::vector<std::string> strings(const Json& a) {
    std::vector<std::string> out;
    for (const auto& s : a) out.push_back(s.get<std::string>());
    return out;
}

PaperOverride override_from(const Json& j) {
    PaperOverride o;
    for (const auto& m : j["mechanics"]) o.mechanics.push_back(mechanic_summary_from_json(m));
    o.supplement = j.value("supplement", "");
    if (j.contains("bodyMd")) o.bodyMd = j["bodyMd"].get<std::string>();
    if (j.contains("issues")) o.issues = strings(j["issues"]);
    if (j.contains("debatability"))
        o.debatability = DebatabilitySummary{j["debatability"]["debatable"].get<bool>(), j["debatability"]["armableBothCount"].get<int>()};
    return o;
}

Json drop_kind(const Json& calls) {
    Json out = Json::array();
    for (auto c : calls) {
        c.erase("kind");
        out.push_back(c);
    }
    return out;
}

FlowUtteranceRecord record_from(const Json& j) {
    FlowUtteranceRecord u;
    u.id = j["id"];
    u.sessionId = j["sessionId"];
    u.paperId = j["paperId"];
    u.round = j["round"];
    u.turn = j["turn"];
    u.personaId = j["personaId"];
    u.personaName = j["personaName"];
    u.role = j["role"];
    u.stance = j["stance"];
    u.text = j["text"];
    u.isError = j["isError"];
    return u;
}

Json phase_to_json(const foresight::ForesightPhaseResult& r) {
    Json list = Json::array();
    for (const auto& u : r.utterances) list.push_back(to_json(u));
    return Json{{"proposal", r.proposal}, {"forecast", r.forecast}, {"utterances", list}, {"status", r.status}, {"failures", r.failures}};
}

/** 所有しない shared_ptr (テストの LLM はスタックに置く)。 */
persona_engine::LLMClientPtr borrow(persona_engine::LLMClient& llm) {
    return persona_engine::LLMClientPtr(&llm, [](persona_engine::LLMClient*) {});
}

void run_golden(const Json& sc) {
    w3l::TempFlowDb tmp;
    set_env(sc["env"]);
    foresight::reset_foresight_status_for_test();
    const std::string name = sc["name"].get<std::string>();
    const std::string mode = sc["mode"].get<std::string>();
    const std::string games_dir = tmp.dir + "/games";
    std::filesystem::create_directories(games_dir);

    w3l::Normalizer norm;
    w3l::ReplayLLM llm(sc["calls"], norm);
    std::vector<std::string> logs, warns;
    Json utter_events = Json::array(), vote_events = Json::array(), lookups = Json::array();
    std::vector<ContextVoice> voices;
    if (sc["voices"].is_array())
        for (const auto& v : sc["voices"]) voices.push_back(context_voice_from_json(v));
    const bool throws = sc["onVoteThrows"].get<bool>();

    FlowDirectorDeps common;
    common.rng = w3l::lcg(sc["seed"].get<std::uint32_t>());
    common.gamesDir = games_dir;
    common.log = [&](const std::string& m) { logs.push_back(norm.str(m)); };
    common.warn = [&](const std::string& m) { warns.push_back(norm.str(m)); };
    common.onUtterance = [&](const FlowUtteranceRecord& u) { utter_events.push_back(norm.any(to_json(u))); };
    common.onVote = [&](const VoteEvent& e) {
        vote_events.push_back(norm.any(to_json(e)));
        if (throws) throw std::runtime_error("vote sink down");
    };
    if (sc["voices"].is_array())
        common.listExternalVoices = [&](const std::vector<std::string>& terms, int limit) {
            lookups.push_back(Json{{"terms", terms}, {"limit", limit}});
            return voices;
        };
    const std::string theme = sc["theme"].get<std::string>();
    const auto tags = strings(sc["tags"]);

    Json actual_result;
    Json extra = Json::object();
    if (mode == "dialectic") {
        FlowRunOptions o;
        static_cast<FlowDirectorDeps&>(o) = common;
        o.sessionId = name;
        if (!sc["paperOverride"].is_null()) o.paperOverride = override_from(sc["paperOverride"]);
        if (!sc["proxyParticipants"].is_null()) {
            std::vector<FlowPersona> ps;
            for (const auto& p : sc["proxyParticipants"]) ps.push_back(flow_persona_from_json(p));
            o.proxyParticipants = ps;
        }
        actual_result = norm.any(to_json(dialectic::run_dialectic_flow(llm, theme, tags, o)));
    } else if (mode == "dispatch") {
        DispatchDeps d;
        d.llm = borrow(llm);
        d.rng = common.rng;
        d.gamesDir = common.gamesDir;
        d.log = common.log;
        d.warn = common.warn;
        d.onUtterance = common.onUtterance;
        d.onVote = common.onVote;
        d.listExternalVoices = common.listExternalVoices;
        d.sessionId = sc["sessionId"].get<std::string>();
        if (!sc["paperOverride"].is_null()) d.paperOverride = override_from(sc["paperOverride"]);
        DispatchInput in;
        in.theme = theme;
        in.tags = tags;
        in.flow = sc["flow"].get<std::string>();
        const auto r = dispatch_flow(in, d);
        actual_result = norm.any(Json{{"kind", r.kind}, {"flow", r.flow}, {"result", to_json(*r.result)}});
        const auto st = foresight::get_foresight_status(*d.sessionId);
        W3L_SAME(st ? foresight::to_json(*st) : Json(nullptr), sc["foresightStatus"]);
    } else if (mode == "improvement") {
        ImprovementFlowDeps d;
        static_cast<FlowDirectorDeps&>(d) = common;
        d.sessionId = name;
        const auto current = negative_baseline_vector();
        d.currentVector = current;
        if (!sc["opinionToVector"].is_null()) {
            const auto gap = compute_design_gap(current, build_target_vector({}));
            std::vector<double> a = current, b = current;
            for (std::size_t k = 0; k < current.size(); ++k) {
                a[k] = current[k] + gap[k];
                b[k] = current[k] + 2 * gap[k];
            }
            d.opinionToVector = [a, b, current](const std::string& t) { return t == "発話A" ? a : t == "発話B" ? b : current; };
        }
        actual_result = norm.any(to_json(run_improvement_flow(llm, theme, tags, d)));
    } else if (mode == "sparring") {
        SparringDeps d;
        d.llm = borrow(llm);
        d.rng = common.rng;
        d.gamesDir = common.gamesDir;
        d.log = common.log;
        d.warn = common.warn;
        d.onUtterance = common.onUtterance;
        d.listExternalVoices = common.listExternalVoices;
        d.responsesPerTurn = name == "sparring" ? 2 : 1;
        SparringSession session(theme, tags, d);
        session.start();
        Json turns = Json::array();
        for (const auto& text : sc["userTexts"]) turns.push_back(norm.any(to_json(session.submit_user(text.get<std::string>()))));
        W3L_SAME(turns, sc["turns"]);
        actual_result = norm.any(Json{{"sessionId", session.session_id()}, {"paperId", session.paper_id()}, {"ended", session.is_ended()}});
    } else {
        const Json& opts = sc["foresightOpts"];
        std::optional<std::string> body;
        if (opts.contains("body")) body = opts["body"].get<std::string>();
        Json committed = Json::array();
        foresight::ForesightPhaseArgs pa;
        pa.theme = theme;
        pa.sessionId = "s-fs";
        pa.paperId = "p-fs";
        pa.flow = "discussion";
        for (const auto& u : sc["mainUtterances"]) pa.utterances.push_back(record_from(u));
        pa.conclusion = opts.value("conclusion", std::string("ガチャは続く"));
        pa.turns = opts["turns"].get<double>();
        if (opts.contains("model")) pa.model = opts["model"].get<std::string>();
        if (opts.contains("forecastModel")) pa.forecastModel = opts["forecastModel"].get<std::string>();
        if (opts.contains("context")) {
            foresight::ForesightContext c;
            c.paperMd = opts["context"]["paperMd"];
            for (const auto& v : opts["context"]["voices"]) c.voices.push_back({v["content"], v["source"]});
            pa.context = c;
        }
        pa.commit = [&](const FlowUtteranceRecord& r) { committed.push_back(norm.any(to_json(r))); };
        if (body) pa.paperBody = foresight::PaperBodyAccess{[&] { return body; }, [&](const std::string& md) { body = md; }};
        pa.log = common.log;
        pa.warn = common.warn;
        const auto r = foresight::run_foresight_phase(llm, pa);
        actual_result = r ? norm.any(phase_to_json(*r)) : Json(nullptr);
        W3L_SAME(committed, sc["committed"]);
        W3L_SAME(body ? Json(*body) : Json(nullptr), sc["body"]);
    }

    CHECK_EQ(llm.calls.size(), sc["calls"].size());
    W3L_SAME(llm.calls, drop_kind(sc["calls"]));
    W3L_SAME(Json(logs), sc["logs"]);
    W3L_SAME(Json(warns), sc["warns"]);
    W3L_SAME(utter_events, sc["utterEvents"]);
    W3L_SAME(vote_events, sc["voteEvents"]);
    W3L_SAME(lookups, sc["lookups"]);
    W3L_SAME(actual_result, sc["result"]);
    if (mode == "foresight") return;
    flow::db::reset_flow_db_for_test();
    sql::OpenOptions ro;
    ro.readonly = true;
    auto db = sql::Database::open(tmp.path, ro);
    const Json dumped = w3l::dump_db(*db, norm);
    for (const auto& [table, rows] : sc["db"].items()) {
        if (!dumped.contains(table)) throw test::Failure(name + ": table missing in C++ DB: " + table);
        W3L_SAME(dumped[table], rows);
    }
    for (const auto& [table, rows] : dumped.items())
        if (!sc["db"].contains(table)) throw test::Failure(name + ": extra table in C++ DB: " + table);
    db->close();
}

/** golden の DB から 1 表を取り出す。 */
const Json& golden_run(const std::string& name) {
    for (const auto& r : golden()["runs"])
        if (r["name"] == name) return r;
    throw test::Failure("no golden run: " + name);
}

FlowPersona persona_from(const Json& j) { return flow_persona_from_json(j); }

FlowDirectorResult fake_result(const std::string& session_id) {
    FlowDirectorResult r;
    r.sessionId = session_id;
    r.paperId = "p-" + session_id;
    r.rounds = 2;
    r.conclusion = "結論X";
    r.concluded = true;
    auto add = [&](const std::string& pid, const std::string& name, const std::string& role, const std::string& stance, int round) {
        FlowUtteranceRecord u;
        u.id = crypto::uuid_v4();
        u.sessionId = session_id;
        u.paperId = r.paperId;
        u.round = round;
        u.turn = static_cast<int>(r.utterances.size());
        u.personaId = pid;
        u.personaName = name;
        u.role = role;
        u.stance = stance;
        u.text = name + " の発言";
        r.utterances.push_back(u);
    };
    add("f", "司会", "facilitator", "neutral", 1);
    add("a", "賛成A", "debater", "pro", 1);
    add("b", "反対B", "debater", "con", 2);
    return r;
}

/** 未来予測の段を判定して応答する LLM (foresight.test.ts の makeLlm)。 */
w3l::FnLLM foresight_llm(std::vector<std::string>& prompts) {
    return w3l::FnLLM([&prompts](const persona_engine::LLMInvokeArgs& a) {
        prompts.push_back(a.prompt);
        const std::string sys = a.system.value_or("");
        if (str::contains(sys, "破壊可能な一手を検討する")) return persona_engine::LLMResult::success("【破壊可能な一手】無料で配る");
        if (str::contains(sys, "条件付きのシナリオで示す")) return persona_engine::LLMResult::success("【3年後】b");
        return persona_engine::LLMResult::success(str::contains(sys, "【改革派") ? "壊せる" : "守るべき");
    });
}

} // namespace

// ─── golden: 進行 ─────────────────────────────────────────────────

TEST_CASE("golden: dialectic / dispatch / 改善 / 壁打ち / 未来予測を Node と同じ応答で最後まで回す") {
    for (const auto& sc : golden()["runs"]) {
        try {
            run_golden(sc);
        } catch (const std::exception& e) {
            throw test::Failure("[" + sc["name"].get<std::string>() + "] " + e.what());
        }
    }
    set_env(Json::object());
    foresight::reset_foresight_status_for_test();
}

TEST_CASE("golden の中身: dialectic-driver.test.ts / proxy-debate.test.ts の観点") {
    // シナリオ 1: 4 表整合・計測型収束・止揚の批准
    {
        const Json db = golden_run("dlx-basic")["db"];
        CHECK_EQ(db["flow_issue"].size(), 3u);
        CHECK_EQ(db["flow_issue"][0]["status"], Json("concluded"));
        CHECK_EQ(db["flow_issue"][1]["status"], Json("open"));
        CHECK_EQ(db["flow_tension"][0]["status"], Json("synthesized"));
        CHECK_EQ(db["flow_synthesis"][0]["kind"], Json("aufheben"));
        CHECK_EQ(db["flow_synthesis"][0]["status"], Json("ratified"));
        int rebuts = 0, concedes = 0, claims = 0;
        for (const auto& u : db["flow_utterance"]) {
            if (u["act"] == "rebut") ++rebuts;
            if (u["act"] == "concede") ++concedes;
            if (u["act"] == "claim") ++claims;
        }
        CHECK_EQ(claims, 6);
        CHECK_EQ(rebuts, 8);
        CHECK_EQ(concedes, 4);
    }
    // 事実対立 (証拠なし) は unresolved_fact で結論に明記
    CHECK(str::contains(golden_run("dlx-fact-unresolved")["result"]["conclusion"].get<std::string>(), "未解決の事実問題"));
    // 批准 reject → 修正 2 回で agreed_disagree
    CHECK_EQ(golden_run("dlx-reject")["db"]["flow_synthesis"][0]["revision"], Json(2));
    CHECK_EQ(golden_run("dlx-reject")["db"]["flow_tension"][0]["status"], Json("agreed_disagree"));
    // 人が指定した論点は打ち切らない
    CHECK_EQ(golden_run("dlx-human-issues")["result"]["rounds"], Json(3));
    // 代理: 対立しない論点は定立を作らない・束に無い根拠の反論は問いに格下げ
    {
        const Json db = golden_run("proxy-e2e")["db"];
        CHECK_EQ(db["flow_position"].size(), 2u);
        bool rebut = false, question = false, notice = false;
        for (const auto& u : db["flow_utterance"]) {
            if (u["act"] == "rebut") rebut = true;
            if (u["act"] == "question") question = true;
            if (str::contains(u["text"].get<std::string>(), "AI の代理")) notice = true;
        }
        CHECK(!rebut);
        CHECK(question);
        CHECK(notice);
    }
    // dispatch の後段: 未来予測が付く (一部失敗)
    CHECK(golden_run("dispatch-dialectic-foresight")["result"]["result"].contains("foresight"));
    // 機密: 外部の声の検索を一度も呼ばない
    CHECK_EQ(golden_run("dispatch-improvement-confidential")["lookups"].size(), 0u);
    // 改善: 効果予測の大きい発話B が世論 / fallback は lexicon-fallback
    const Json scores = golden_run("improvement-fallback")["db"]["improvement_score"];
    for (const auto& s : scores) CHECK_EQ(s["method"], Json("lexicon-fallback"));
}

// ─── golden: 純関数 ───────────────────────────────────────────────

TEST_CASE("golden: vote-display (scores は票数表記にしない)") {
    for (const auto& c : golden()["voteDisplay"]) {
        VoteDisplay e;
        e.kind = c["event"]["kind"];
        for (const auto& [k, v] : c["event"]["tally"].items()) e.tally.emplace_back(k, v.get<double>());
        if (!c["event"]["winner"].is_null()) e.winner = c["event"]["winner"].get<std::string>();
        W3L_SAME(Json(build_vote_ranking_lines(e, [](const std::string& id) { return "名-" + id; })), c["lines"]);
        CHECK_EQ(has_visible_vote_result(e), c["visible"].get<bool>());
        CHECK_EQ(should_stack_vote_reactions(e.kind), c["stack"].get<bool>());
        CHECK_EQ(vote_notice_title(3, e.kind), c["title"].get<std::string>());
    }
}

TEST_CASE("golden: dialectic 決着表 (settlement_to_round_summary / collect_unresolved_facts)") {
    std::map<std::string, std::string> names;
    for (const auto& [k, v] : golden()["settlementNames"].items()) names[k] = v;
    std::vector<dialectic::IssueSettlement> all;
    for (const auto& c : golden()["settlements"]) {
        const Json& s = c["settlement"];
        dialectic::IssueSettlement st;
        st.issue.id = s["issue"]["id"];
        st.issue.title = s["issue"]["title"];
        st.issue.ordinal = s["issue"]["ordinal"];
        for (const auto& p : s["positions"]) {
            dialectic::PositionRecord pr;
            pr.id = p["id"];
            pr.personaId = p["personaId"];
            pr.stance = p["stance"];
            pr.claim = p["claim"];
            st.positions.push_back(pr);
        }
        for (const auto& t : s["tensions"]) {
            dialectic::TensionRecord tr;
            tr.id = t["id"];
            tr.status = t["status"];
            if (!t["resolutionNote"].is_null()) tr.resolutionNote = t["resolutionNote"].get<std::string>();
            st.tensions.push_back(tr);
        }
        for (const auto& x : s["syntheses"]) {
            dialectic::SynthesisRecord sr;
            sr.tensionId = x["tensionId"];
            sr.text = x["text"];
            if (!x["elevates"].is_null()) sr.elevates = x["elevates"].get<std::string>();
            st.syntheses.push_back(sr);
        }
        if (!s["refutation"].is_null()) st.refutation = s["refutation"].get<std::string>();
        if (s.contains("undecided")) st.undecided = s["undecided"].get<std::string>();
        const std::optional<double> share = c["share"].is_null() ? std::nullopt : std::optional<double>(c["share"].get<double>());
        W3L_SAME(to_json(dialectic::settlement_to_round_summary(st, names, share)), c["summary"]);
        all.push_back(st);
    }
    W3L_SAME(Json(dialectic::collect_unresolved_facts(all)), golden()["unresolvedFacts"]);
}

TEST_CASE("golden: pick_side_speaker / ensure_debaters") {
    std::vector<FlowPersona> speakers;
    for (const auto& p : golden()["speakers"]) speakers.push_back(persona_from(p));
    for (const auto& c : golden()["pickSide"]) {
        std::map<std::string, int> counts;
        for (const auto& [k, v] : c["counts"].items()) counts[k] = v;
        const auto* p = dialectic::pick_side_speaker(speakers, c["side"].get<std::string>(), counts);
        W3L_SAME(p ? Json(p->id) : Json(nullptr), c["picked"]);
    }
    for (const auto& c : golden()["ensureDebaters"]) {
        std::vector<FlowPersona> cast;
        for (const auto& p : c["cast"]) cast.push_back(persona_from(p));
        std::vector<std::string> warns;
        if (c["ok"].get<bool>()) {
            const auto r = dialectic::ensure_debaters(cast, [&](const std::string& m) { warns.push_back(m); });
            W3L_SAME(to_json(r.pro), c["pro"]);
            W3L_SAME(to_json(r.con), c["con"]);
            Json result = Json::array();
            for (const auto& p : r.cast) result.push_back(to_json(p));
            W3L_SAME(result, c["result"]);
        } else {
            try {
                (void)dialectic::ensure_debaters(cast, [&](const std::string& m) { warns.push_back(m); });
                throw test::Failure("ensure_debaters should throw");
            } catch (const std::runtime_error& e) {
                CHECK_EQ(std::string(e.what()), c["error"].get<std::string>());
            }
        }
        W3L_SAME(Json(warns), c["warns"]);
    }
}

TEST_CASE("golden: parse_flow_kind (entrypoints.test.ts / forum-flow-tags.test.ts)") {
    for (const auto& c : golden()["flowKinds"]) {
        const auto k = parse_flow_kind(std::string_view(c["label"].get_ref<const std::string&>()));
        W3L_SAME(k ? Json(*k) : Json(nullptr), c["kind"]);
    }
    CHECK(!parse_flow_kind(std::nullopt));
}

TEST_CASE("golden: 未来予測の話者割当・往復・段階・投稿の分割") {
    for (const auto& c : golden()["casts"]) {
        std::vector<FlowUtteranceRecord> us;
        for (const auto& u : c["utterances"]) us.push_back(record_from(u));
        const auto cast = foresight::build_foresight_cast(us);
        W3L_SAME(cast ? foresight::to_json(*cast) : Json(nullptr), c["cast"]);
    }
    for (const auto& c : golden()["normalizeTurns"]) CHECK_EQ(foresight::normalize_camp_turns(c["n"].get<double>()), c["v"].get<int>());
    CHECK_EQ(foresight::normalize_camp_turns(std::nan("")), golden()["normalizeTurnsSpecial"]["nan"].get<int>());
    // JS は Infinity をそのまま返す (無限ループ)。C++ は MAX_CAMP_TURNS で止める (暴走ガード)。
    CHECK_EQ(foresight::normalize_camp_turns(INFINITY), foresight::MAX_CAMP_TURNS);
    for (const auto& c : golden()["stages"]) CHECK_EQ(foresight::camp_stage_at(c["i"].get<int>()), c["stage"].get<std::string>());
    std::vector<FlowUtteranceRecord> main_utterances;
    for (const auto& u : golden()["casts"][0]["utterances"]) main_utterances.push_back(record_from(u));
    const auto cast = foresight::build_foresight_cast(main_utterances);
    for (const auto& c : golden()["turnOrders"]) {
        Json ids = Json::array();
        for (const auto& s : foresight::build_camp_turn_order(cast->speakers, c["turns"].get<double>())) ids.push_back(s.personaId);
        W3L_SAME(ids, c["order"]);
    }
    std::vector<foresight::ForesightSpeaker> reform_only;
    for (const auto& s : cast->speakers)
        if (s.camp == "reform") reform_only.push_back(s);
    CHECK_EQ(foresight::build_camp_turn_order(reform_only, 4).size(), golden()["turnOrderOneCamp"].get<std::size_t>());
    for (const auto& c : golden()["splits"])
        W3L_SAME(Json(foresight::split_for_post(c["text"].get<std::string>(), c["limit"].get<std::size_t>())), c["chunks"]);
    for (const auto& c : golden()["labels"]) W3L_SAME(Json(foresight::label_parts(strings(c["chunks"]))), c["labeled"]);
}

// ─── dispatch の振り分け (注入ドライバ) ─────────────────────────────

TEST_CASE("dispatch: ラベルごとに正しいドライバ / flow 必須 (entrypoints.test.ts)") {
    w3l::TempFlowDb tmp;
    set_env(Json::object());
    std::vector<std::string> calls;
    w3l::FnLLM llm([](const persona_engine::LLMInvokeArgs&) { return persona_engine::LLMResult::success(""); });
    DispatchDeps d;
    d.llm = borrow(llm);
    d.drivers.discussion = [&](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps&) {
        calls.push_back("discussion");
        return fake_result("s");
    };
    d.drivers.improvement = [&](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps&) {
        calls.push_back("improvement");
        return fake_result("s");
    };
    auto in = [](std::string flow) {
        DispatchInput i;
        i.theme = "t";
        i.flow = std::move(flow);
        return i;
    };
    CHECK_EQ(dispatch_flow(in("議論"), d).kind, std::string("discussion"));
    CHECK_EQ(dispatch_flow(in("改善"), d).kind, std::string("improvement"));
    CHECK_EQ(calls, (std::vector<std::string>{"discussion", "improvement"}));
    for (const char* bad : {"学習", "壁打ち", "sparring", "", "雑談"}) {
        try {
            (void)dispatch_flow(in(bad), d);
            throw test::Failure(std::string("should reject: ") + bad);
        } catch (const FlowTypeRequiredError&) {
        }
    }
    // llm が無い呼び出しは受けない
    DispatchDeps none;
    CHECK_THROWS(dispatch_flow(in("議論"), none));
    // rounds / turnsPerRound / personaIds / state がドライバに渡る
    auto state = std::make_shared<FlowRunState>();
    d.state = state;
    d.drivers.discussion = [&](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps& fd) {
        CHECK(fd.state == state);
        CHECK(fd.rounds && *fd.rounds == 3);
        CHECK(fd.personaIds && fd.personaIds->size() == 1);
        return fake_result("s");
    };
    DispatchInput i = in("企画/議論");
    i.rounds = 3;
    i.personaIds = std::vector<std::string>{"p1"};
    (void)dispatch_flow(i, d);
}

TEST_CASE("dispatch 後段: 未来予測 (タグ / 機密・内部 / 未収束 / 例外 / 無効) (foresight.test.ts)") {
    w3l::TempFlowDb tmp;
    set_env(Json::object());
    foresight::reset_foresight_status_for_test();
    std::vector<std::string> prompts;
    auto llm = foresight_llm(prompts);
    std::vector<FlowUtteranceRecord> notified;
    std::optional<std::string> pending_during_run;
    DispatchDeps d;
    d.llm = borrow(llm);
    d.sessionId = "s-dispatch";
    d.onUtterance = [&](const FlowUtteranceRecord& u) { notified.push_back(u); };
    d.listExternalVoices = [](const std::vector<std::string>&, int) { return std::vector<ContextVoice>{{"声その1", "niconico", std::nullopt}}; };
    d.log = [](const std::string&) {};
    d.warn = [](const std::string&) {};
    d.drivers.discussion = [&](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps&) {
        const auto st = foresight::get_foresight_status("s-dispatch");
        pending_during_run = st ? std::optional<std::string>(st->state) : std::nullopt;
        return fake_result("s-dispatch");
    };
    auto in = [](std::vector<std::string> tags) {
        DispatchInput i;
        i.theme = "t";
        i.flow = "議論";
        i.tags = std::move(tags);
        return i;
    };

    const auto untagged = dispatch_flow(in({"開発"}), d);
    CHECK(!untagged.result->foresight);
    CHECK(notified.empty());
    CHECK(!foresight::get_foresight_status("s-dispatch"));

    const auto on = dispatch_flow(in({"未来予測"}), d);
    CHECK_EQ(pending_during_run.value_or(""), std::string("pending"));
    CHECK(on.result->foresight && str::contains(on.result->foresight->forecast, "【3年後】"));
    CHECK_EQ(on.result->utterances.size(), 3u + notified.size());
    CHECK(notified.size() >= 3);
    CHECK_EQ(on.result->conclusion, std::string("結論X"));
    CHECK_EQ(foresight::get_foresight_status("s-dispatch")->state, std::string("done"));
    CHECK(std::any_of(prompts.begin(), prompts.end(), [](const std::string& p) { return str::contains(p, "声その1"); }));
    // 未来予測の発話は flow_utterance に永続される
    {
        auto db = flow::db::get_flow_db();
        CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM flow_utterance WHERE session_id = 's-dispatch'")->i64("n"),
                 static_cast<std::int64_t>(notified.size()));
    }

    // 内部/機密は本議論にも未来予測にも蓄積済みの外部の声を渡さない
    for (const char* deny : {"内部", "機密"}) {
        const std::size_t before = prompts.size();
        bool driver_has_voices = true;
        DispatchDeps gd = d;
        gd.drivers.discussion = [&](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps& fd) {
            driver_has_voices = static_cast<bool>(fd.listExternalVoices);
            return fake_result("s-dispatch");
        };
        const auto gated = dispatch_flow(in({"未来予測", deny}), gd);
        CHECK(!driver_has_voices);
        CHECK(gated.result->foresight.has_value());
        for (std::size_t k = before; k < prompts.size(); ++k) CHECK(!str::contains(prompts[k], "声その1"));
    }

    // 未収束なら行わない
    notified.clear();
    DispatchDeps ud = d;
    ud.drivers.discussion = [](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps&) {
        auto r = fake_result("s-dispatch");
        r.conclusion = "";
        r.concluded = false;
        return r;
    };
    const auto unconcluded = dispatch_flow(in({"未来予測"}), ud);
    CHECK(!unconcluded.result->foresight);
    CHECK(notified.empty());
    CHECK(foresight::get_foresight_status("s-dispatch") == (foresight::ForesightStatus{"skipped", std::string("本議論が収束していない")}));

    // 本議論の例外は状態を失敗にして投げ直す
    DispatchDeps ed = d;
    ed.drivers.discussion = [](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps&) -> FlowDirectorResult {
        throw std::runtime_error("engine down");
    };
    try {
        (void)dispatch_flow(in({"未来予測"}), ed);
        throw test::Failure("should rethrow");
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("engine down"));
    }
    CHECK_EQ(foresight::get_foresight_status("s-dispatch")->state, std::string("failed"));

    // 機能が無効ならタグがあっても付けない
    set_env(Json{{"DISCUTERE_FLOW_FORESIGHT_ENABLED", "false"}});
    foresight::reset_foresight_status_for_test();
    notified.clear();
    const auto off = dispatch_flow(in({"未来予測"}), d);
    CHECK(!off.result->foresight);
    CHECK(notified.empty());
    CHECK(!foresight::get_foresight_status("s-dispatch"));
    set_env(Json::object());
    foresight::reset_foresight_status_for_test();
}

TEST_CASE("未来予測フェーズ: ペーパー追記の失敗も一部失敗に数える / commit 必須") {
    w3l::TempFlowDb tmp;
    set_env(Json::object());
    std::vector<std::string> prompts;
    auto llm = foresight_llm(prompts);
    foresight::ForesightPhaseArgs pa;
    pa.theme = "t";
    pa.sessionId = "s";
    pa.paperId = "p";
    pa.flow = "discussion";
    pa.utterances = fake_result("s").utterances;
    pa.conclusion = "c";
    pa.turns = 2;
    pa.commit = [](const FlowUtteranceRecord&) {};
    pa.paperBody = foresight::PaperBodyAccess{[] { return std::optional<std::string>("# 結論\nY"); },
                                              [](const std::string&) { throw std::runtime_error("db locked"); }};
    const auto r = foresight::run_foresight_phase(llm, pa);
    CHECK_EQ(r->status, std::string("degraded"));
    CHECK_EQ(r->failures, 1);
    pa.commit = nullptr;
    CHECK_THROWS(foresight::run_foresight_phase(llm, pa));
    // 話者 1 人なら行わない
    pa.commit = [](const FlowUtteranceRecord&) {};
    pa.utterances.resize(2);
    CHECK(!foresight::run_foresight_phase(llm, pa));
}

// ─── 進行状態・並行 ─────────────────────────────────────────────

TEST_CASE("進行状態: dialectic を別スレッドから読み、request_stop で FlowStopped (発話は残り結論は作らない)") {
    w3l::TempFlowDb tmp;
    set_env(golden_run("dlx-basic")["env"]);
    w3l::Normalizer norm;
    w3l::ReplayLLM llm(golden_run("dlx-basic")["calls"], norm);
    auto state = std::make_shared<FlowRunState>();
    std::atomic<int> seen{0};
    FlowRunOptions o;
    o.sessionId = "dlx-stop";
    o.rng = w3l::lcg(1234);
    o.gamesDir = tmp.dir + "/games";
    o.log = [](const std::string&) {};
    o.warn = [](const std::string&) {};
    o.state = state;
    o.onUtterance = [&](const FlowUtteranceRecord&) {
        if (++seen == 5) {
            std::thread([state] { state->request_stop(); }).join();
        }
    };
    bool stopped = false;
    try {
        (void)dialectic::run_dialectic_flow(llm, "ガチャ緩和の是非", {}, o);
    } catch (const FlowStopped&) {
        stopped = true;
    }
    CHECK(stopped);
    const auto snap = state->snapshot();
    CHECK_EQ(snap.phase, std::string("stopped"));
    CHECK_EQ(snap.utterances, 5);
    CHECK_EQ(snap.round, 1);
    CHECK_EQ(snap.rounds, 3);
    auto db = flow::db::get_flow_db();
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM flow_utterance WHERE session_id = 'dlx-stop'")->i64("n"), 5);
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM flow_conclusion WHERE session_id = 'dlx-stop'")->i64("n"), 0);
    set_env(Json::object());
}

TEST_CASE("壁打ち: 同じセッションへの並行の submit_user は直列化され、暴走ガードを超えない / start 前は拒否") {
    w3l::TempFlowDb tmp;
    set_env(Json{{"DISCUTERE_FLOW_PERSONA_COUNT", "2"}, {"DISCUTERE_FLOW_SPARRING_MAX_TURNS", "5"}});
    std::atomic<int> llm_calls{0};
    w3l::FnLLM llm([&](const persona_engine::LLMInvokeArgs& a) {
        ++llm_calls;
        if (str::contains(a.prompt, "found=false")) return persona_engine::LLMResult::success(R"({"found":false,"summary":""})");
        return persona_engine::LLMResult::success("応答");
    });
    SparringDeps d;
    d.llm = borrow(llm);
    d.rng = w3l::lcg(3);
    d.gamesDir = tmp.dir + "/no-games";
    d.log = [](const std::string&) {};
    d.warn = [](const std::string&) {};
    auto session = std::make_shared<SparringSession>("テーマ", std::vector<FlowTag>{}, d);
    CHECK_THROWS(session->submit_user("早すぎる"));
    session->start();
    std::atomic<int> ended{0}, responses{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            for (int k = 0; k < 5; ++k) {
                const auto r = session->submit_user("発言 " + std::to_string(t) + "-" + std::to_string(k));
                if (r.kind == "ended") ++ended;
                if (r.kind == "responses") responses += static_cast<int>(r.utterances.size());
            }
        });
    for (auto& th : threads) th.join();
    CHECK(session->is_ended());
    CHECK_EQ(responses.load(), 5);  // sparringMaxTurns
    CHECK(ended.load() >= 1);
    auto db = flow::db::get_flow_db();
    const auto ai = db->get("SELECT COUNT(*) AS n FROM flow_utterance WHERE session_id = ? AND role != 'user'", {session->session_id()})->i64("n");
    CHECK_EQ(ai, 5);
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM vote WHERE session_id = ?", {session->session_id()})->i64("n"), 0);
    set_env(Json::object());
}

// ─── 境界 ───────────────────────────────────────────────────────

TEST_CASE("境界: split_for_post の上限ちょうど / +1 / 0 / 巨大な入力") {
    CHECK_EQ(foresight::split_for_post(std::string(1780, 'a')).size(), 1u);
    CHECK_EQ(foresight::split_for_post(std::string(1781, 'a')).size(), 2u);
    CHECK_EQ(foresight::split_for_post(std::string(1779, 'a') + "\nb").size(), 1u);  // 1779 + 1 + 1 = 1781 > 1780
    CHECK_EQ(foresight::split_for_post(std::string(1778, 'a') + "\nb").size(), 1u);
    CHECK_EQ(foresight::split_for_post(std::string(1779, 'a') + "\nbb").size(), 2u);
    CHECK_EQ(foresight::split_for_post("abc", 0).size(), 3u);  // 0 は 1 として扱う
    // 壊れた UTF-8 も落ちない
    (void)foresight::split_for_post(std::string("\xff\xfe\x80", 3), 1);
    // 巨大な入力 (8MB) を線形に分ける
    std::string huge;
    for (int i = 0; i < 80000; ++i) huge += std::string(99, 'x') + "\n";
    const auto chunks = foresight::split_for_post(huge);
    std::size_t total = 0;
    for (const auto& c : chunks) {
        CHECK(str::count_codepoints(c) <= foresight::POST_LIMIT);
        total += c.size();
    }
    CHECK(total + chunks.size() >= huge.size() - 1);
    CHECK_EQ(foresight::normalize_camp_turns(1e18), foresight::MAX_CAMP_TURNS);
    CHECK_EQ(foresight::normalize_camp_turns(foresight::MAX_CAMP_TURNS + 1), foresight::MAX_CAMP_TURNS);
    CHECK_EQ(foresight::camp_stage_at(-1), std::string("conditions"));
}

TEST_CASE("境界: parse_flow_kind / vote 表示への巨大な入力") {
    std::string big(1u << 20, 'x');
    CHECK(!parse_flow_kind(std::string_view(big)));
    CHECK_EQ(parse_flow_kind(std::string_view(big + "改善")).value_or(""), std::string("improvement"));
    VoteDisplay e;
    e.kind = "votes";
    for (int i = 0; i < 100000; ++i) e.tally.emplace_back("u" + std::to_string(i), i % 7);
    const auto lines = build_vote_ranking_lines(e, [](const std::string& id) { return id; });
    CHECK(lines.size() < 100000u);  // 0 票は出さない
    CHECK(str::starts_with(lines[0], "1. "));
    e.kind = "scores";
    e.tally = {{"a", std::nan("")}, {"b", 1.0}};
    CHECK_EQ(build_vote_ranking_lines(e, nullptr).size(), 2u);
}

TEST_CASE("境界: 壊れた・巨大な・失敗する LLM 応答でも dialectic は完走する (degrade)") {
    for (int variant = 0; variant < 3; ++variant) {
        w3l::TempFlowDb tmp;
        set_env(golden_run("dlx-basic")["env"]);
        std::string garbage = variant == 0 ? std::string(2u << 20, '{') : variant == 1 ? std::string("[1,2,{\"a\":") : std::string();
        w3l::FnLLM llm([&](const persona_engine::LLMInvokeArgs&) {
            if (variant == 2) return persona_engine::LLMResult::failure("down");
            return persona_engine::LLMResult::success(garbage);
        });
        FlowRunOptions o;
        o.sessionId = "dlx-garbage";
        o.rng = w3l::lcg(5);
        o.gamesDir = tmp.dir + "/games";
        o.log = [](const std::string&) {};
        o.warn = [](const std::string&) {};
        const auto r = dialectic::run_dialectic_flow(llm, "壊れたテーマ", {}, o);
        CHECK(r.rounds >= 1);
        CHECK(llm.count > 0);
        auto db = flow::db::get_flow_db();
        CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM flow_conclusion WHERE session_id = 'dlx-garbage'")->i64("n"), 1);
    }
    set_env(Json::object());
}

TEST_CASE("run_ground_exchange: 必須の引数が無ければ拒否 / ensure_debaters は昇格できなければ例外") {
    dialectic::GroundExchangeArgs a;
    CHECK_THROWS(dialectic::run_ground_exchange(a));
    FlowPersona f;
    f.id = "f";
    f.role = "facilitator";
    CHECK_THROWS(dialectic::ensure_debaters({f}, nullptr));
}
