#pragma once
// w3l_director_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w3-L: director (rounds エンジン)・persona_turn・facilitator_turn・conclusion のテスト。
//   - golden: Node と同じ偽の LLM 応答で 1 議論を最後まで回し、LLM への呼び出し列・DB に残る全行・ペーパー md・
//     通知・log/warn・戻り値が一致すること (tests/golden/w3-L-flow.gen.ts)。
//   - 結合: tests/flow/discussion.test.ts / possession-vote.test.ts / turn-scheduler.test.ts の director 部分。
//   - 進行状態 (C++ 版の追加): 別スレッドからの問い合わせと停止。
#include "check.hpp"
#include "flow/w3l_support.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <limits>
#include <thread>

#include "base/sqlite.hpp"
#include "base/strings.hpp"
#include "flow/conclusion.hpp"
#include "flow/director.hpp"
#include "flow/facilitator_turn.hpp"
#include "flow/investigate.hpp"
#include "flow/persona_pool.hpp"
#include "flow/sentiment_vector.hpp"

using namespace di;
using namespace di::flow;
using w3l::Json;

namespace w3l_director_support {

const std::vector<std::string> kAllEnv = {
    "DISCUTERE_FLOW_ROUNDS",
    "DISCUTERE_FLOW_TURNS_PER_ROUND",
    "DISCUTERE_FLOW_PERSONA_COUNT",
    "DISCUTERE_FLOW_VOTER_COUNT",
    "DISCUTERE_FACILITATOR_AUFHEBUNG_TARGET",
    "DISCUTERE_FLOW_CONVERGE_SHARE",
    "DISCUTERE_FLOW_PAPER_REFINE_ENABLED",
    "DISCUTERE_FLOW_PAPER_REFINE_MODEL",
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
    if (j.contains("debatability")) o.debatability = DebatabilitySummary{j["debatability"]["debatable"].get<bool>(), j["debatability"]["armableBothCount"].get<int>()};
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

void run_golden(const Json& sc) {
    w3l::TempFlowDb tmp;
    set_env(sc["env"]);
    const std::string games_dir = tmp.dir + "/games";
    std::filesystem::create_directories(games_dir);
    if (!sc["poolVector"].is_null()) {
        PoolPersona p;
        p.id = "pool-possess-1";
        p.name = "論者#test01";
        p.role = "opinion";
        for (const auto& x : sc["poolVector"]) p.affectVector.push_back(x.get<double>());
        p.origin = "adopted";
        p.learningSource = "youtube";
        p.sourceSpeakerId = "ext:youtube:tester";
        p.typicality = 0.95;
        p.polarityBias = 0.6;
        p.affectDispersion = 0.2;
        insert_pool_persona(p);
    }

    w3l::Normalizer norm;
    w3l::ReplayLLM llm(sc["calls"], norm);
    std::vector<std::string> logs, warns;
    Json utter_events = Json::array(), vote_events = Json::array(), lookups = Json::array();
    std::vector<ContextVoice> voices;
    for (const auto& v : sc["voices"]) voices.push_back(context_voice_from_json(v));

    FlowRunOptions o;
    const Json& opts = sc["options"];
    o.sessionId = opts["sessionId"].get<std::string>();
    if (opts.contains("rounds")) o.rounds = opts["rounds"].get<double>();
    if (opts.contains("turnsPerRound")) o.turnsPerRound = opts["turnsPerRound"].get<double>();
    if (opts.contains("paperOverride")) o.paperOverride = override_from(opts["paperOverride"]);
    o.rng = w3l::lcg(sc["seed"].get<std::uint32_t>());
    o.gamesDir = games_dir;
    o.listExternalVoices = [&](const std::vector<std::string>& terms, int limit) {
        lookups.push_back(Json{{"terms", terms}, {"limit", limit}});
        return voices;
    };
    o.log = [&](const std::string& m) { logs.push_back(norm.str(m)); };
    o.warn = [&](const std::string& m) { warns.push_back(norm.str(m)); };
    o.onUtterance = [&](const FlowUtteranceRecord& u) { utter_events.push_back(norm.any(to_json(u))); };
    const bool throws = sc["onVoteThrows"].get<bool>();
    o.onVote = [&](const VoteEvent& e) {
        vote_events.push_back(norm.any(to_json(e)));
        if (throws) throw std::runtime_error("vote sink down");
    };
    if (sc["evaluator"] == "scores") {
        o.flow = "improvement";
        o.evaluateRound = [](const RoundEvalContext& ctx) {
            VoteResult r;
            r.kind = "scores";
            for (std::size_t i = 0; i < ctx.candidates.size(); ++i) r.tally.emplace_back(ctx.candidates[i].id, 0.5 - static_cast<double>(i) * 0.25);
            if (!ctx.candidates.empty()) r.winner = ctx.candidates[0].id;
            return r;
        };
    }

    const std::string name = sc["name"].get<std::string>();
    const auto result = run_flow(llm, sc["theme"].get<std::string>(), strings(sc["tags"]), o);

    if (const auto dump = env::get("W3L_DUMP_DIR")) {  // 差分調査用 (golden と並べて比べる)
        Json d = Json::object();
        d["calls"] = llm.calls;
        d["logs"] = logs;
        d["warns"] = warns;
        d["utterEvents"] = utter_events;
        d["voteEvents"] = vote_events;
        d["result"] = norm.any(to_json(result));
        fs::write_file_atomic(*dump + "/" + name + ".json", d.dump(1));
    }
    CHECK_EQ(llm.calls.size(), sc["calls"].size());
    W3L_SAME(llm.calls, drop_kind(sc["calls"]));
    W3L_SAME(Json(logs), sc["logs"]);
    W3L_SAME(Json(warns), sc["warns"]);
    W3L_SAME(utter_events, sc["utterEvents"]);
    W3L_SAME(vote_events, sc["voteEvents"]);
    W3L_SAME(lookups, sc["lookups"]);
    W3L_SAME(norm.any(to_json(result)), sc["result"]);
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

} // namespace w3l_director_support
