#pragma once
// w4p_flow_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
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

namespace w4p_flow_support {

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

} // namespace w4p_flow_support
