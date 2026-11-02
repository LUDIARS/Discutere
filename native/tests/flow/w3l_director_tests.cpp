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

namespace {

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

} // namespace

TEST_CASE("golden: Node と同じ応答で 1 議論を最後まで回す (basic / override / scores)") {
    for (const auto& sc : w3l::golden()["runs"]) {
        try {
            run_golden(sc);
        } catch (const std::exception& e) {
            throw test::Failure("[" + sc["name"].get<std::string>() + "] " + e.what());
        }
    }
    set_env(Json::object());
}

TEST_CASE("golden: build_facilitator_prompt / build_rounds_section") {
    for (const auto& c : w3l::golden()["facilitatorPrompts"]) {
        std::optional<PreviousRoundContext> prev;
        if (!c["prev"].is_null()) {
            PreviousRoundContext p;
            p.round = c["prev"]["round"].get<int>();
            p.summary = c["prev"]["summary"].get<std::string>();
            p.aufhebung = strings(c["prev"]["aufhebung"]);
            if (!c["prev"]["winner"].is_null())
                p.winner = PreviousRoundContext::Winner{c["prev"]["winner"]["personaName"].get<std::string>(), c["prev"]["winner"]["text"].get<std::string>()};
            prev = p;
        }
        CHECK_EQ(build_facilitator_prompt(c["theme"].get<std::string>(), c["round"].get<int>(), prev, strings(c["issues"])), c["prompt"].get<std::string>());
    }
    for (const auto& c : w3l::golden()["roundsSections"]) {
        std::vector<RoundSummary> rounds;
        for (const auto& r : c["rounds"]) rounds.push_back({r["round"].get<int>(), r["summary"].get<std::string>(), strings(r["aufhebung"])});
        CHECK_EQ(build_rounds_section(rounds), c["text"].get<std::string>());
    }
}

TEST_CASE("clamp_count / is_vote_candidate") {
    CHECK_EQ(clamp_count(std::nullopt, 3, 10), 3.0);
    CHECK_EQ(clamp_count(std::numeric_limits<double>::infinity(), 3, 10), 3.0);
    CHECK_EQ(clamp_count(std::numeric_limits<double>::quiet_NaN(), 3, 10), 3.0);
    CHECK_EQ(clamp_count(0, 3, 10), 1.0);
    CHECK_EQ(clamp_count(-5, 3, 10), 1.0);
    CHECK_EQ(clamp_count(10, 3, 10), 10.0);   // 上限ちょうど
    CHECK_EQ(clamp_count(11, 3, 10), 10.0);   // 上限 + 1
    CHECK_EQ(clamp_count(1e300, 3, 10), 10.0);  // 巨大
    CHECK_EQ(clamp_count(2.9, 3, 10), 2.0);
    FlowUtteranceRecord u;
    u.round = 2;
    u.role = "debater";
    CHECK(is_vote_candidate(u, 2));
    CHECK(!is_vote_candidate(u, 1));
    u.isError = true;
    CHECK(!is_vote_candidate(u, 2));
    u.isError = false;
    u.role = "facilitator";
    CHECK(!is_vote_candidate(u, 2));
}

TEST_CASE("結合: rounds × turnsPerRound が回り発話とペーパーが永続される (discussion.test.ts)") {
    w3l::TempFlowDb tmp;
    set_env(Json{{"DISCUTERE_FLOW_ROUNDS", "2"}, {"DISCUTERE_FLOW_TURNS_PER_ROUND", "3"}, {"DISCUTERE_FLOW_PERSONA_COUNT", "3"}});
    w3l::FnLLM llm([](const persona_engine::LLMInvokeArgs&) { return persona_engine::LLMResult::success("テスト発話"); });
    FlowDirectorDeps deps;
    deps.rng = w3l::lcg(1234);
    deps.gamesDir = tmp.dir + "/no-games";
    deps.log = [](const std::string&) {};
    deps.warn = [](const std::string&) {};
    const auto result = run_discussion_flow(llm, "ジャーニーの感情設計", {}, deps);
    auto db = flow::db::get_flow_db();
    const auto rows = db->all("SELECT * FROM flow_utterance WHERE session_id = ?", {result.sessionId});
    CHECK(!rows.empty());
    CHECK(rows.size() <= 8);  // 2 rounds × (3 turns + 開幕 1)
    CHECK_EQ(result.rounds, 2);
    const auto paper = db->get("SELECT * FROM discussion_paper WHERE session_id = ?", {result.sessionId});
    CHECK(paper.has_value());
    CHECK_EQ(paper->str("theme"), std::string("ジャーニーの感情設計"));
    // ペーパー本文は議論の経過 + 結論まで焼き込まれる。
    CHECK(str::contains(paper->str("body_md"), "# 議論の経過"));
    set_env(Json::object());
}

TEST_CASE("結合: LLM 失敗はエラー発話 (is_error=1) と llm_call_log に残る") {
    w3l::TempFlowDb tmp;
    set_env(Json{{"DISCUTERE_FLOW_ROUNDS", "1"}, {"DISCUTERE_FLOW_TURNS_PER_ROUND", "1"}, {"DISCUTERE_FLOW_PERSONA_COUNT", "2"}});
    int n = 0;
    w3l::FnLLM llm([&](const persona_engine::LLMInvokeArgs&) {
        ++n;
        if (n == 1) return persona_engine::LLMResult::success("[]");
        if (n == 2) return persona_engine::LLMResult::failure("API timeout");
        return persona_engine::LLMResult::success("x");
    });
    FlowDirectorDeps deps;
    deps.rng = w3l::lcg(777);
    deps.log = [](const std::string&) {};
    deps.warn = [](const std::string&) {};
    const auto result = run_discussion_flow(llm, "エラーテスト", {}, deps);
    auto db = flow::db::get_flow_db();
    const auto err = db->all("SELECT * FROM flow_utterance WHERE session_id = ? AND is_error = 1", {result.sessionId});
    CHECK(!err.empty());
    CHECK(str::contains(err[0].str("text"), "エラー"));
    CHECK(!db->all("SELECT * FROM llm_call_log WHERE session_id = ?", {result.sessionId}).empty());
    set_env(Json::object());
}

TEST_CASE("結合: 憑依した発話は casual 名を保ち possessionName を持つ (possession-vote.test.ts)") {
    w3l::TempFlowDb tmp;
    set_env(Json::object());
    const std::string theme = "ローグライトの中毒性はどこから来るか";
    PoolPersona p;
    p.id = "pool-possess-1";
    p.name = "論者#test01";
    p.role = "opinion";
    p.affectVector = text_to_vector(theme);
    p.origin = "adopted";
    insert_pool_persona(p);
    w3l::FnLLM llm([](const persona_engine::LLMInvokeArgs&) { return persona_engine::LLMResult::success("なるほど、その視点は面白いと思う。"); });
    std::vector<FlowUtteranceRecord> got;
    std::vector<VoteEvent> votes;
    FlowDirectorDeps deps;
    deps.rng = [] { return 0.3; };
    deps.rounds = 1;
    deps.turnsPerRound = 3;
    deps.log = [](const std::string&) {};
    deps.warn = [](const std::string&) {};
    deps.onUtterance = [&](const FlowUtteranceRecord& u) { got.push_back(u); };
    deps.onVote = [&](const VoteEvent& e) { votes.push_back(e); };
    run_discussion_flow(llm, theme, {}, deps);
    bool possessed = false;
    for (const auto& u : got)
        if (u.possessionName) {
            possessed = true;
            CHECK_EQ(*u.possessionName, std::string("論者#test01"));
            CHECK(u.personaName != "論者#test01");
        }
    CHECK(possessed);
    CHECK_EQ(votes.size(), 1u);
    for (const auto& c : votes[0].candidates) {
        const auto it = std::find_if(got.begin(), got.end(), [&](const auto& u) { return u.id == c.id; });
        CHECK(it != got.end());
        CHECK(it->role != "facilitator");
    }
    // personaIds 指定時は憑依しない
    got.clear();
    deps.personaIds = std::vector<std::string>{"pool-possess-1"};
    run_discussion_flow(llm, theme, {}, deps);
    for (const auto& u : got) CHECK(!u.possessionName.has_value());
}

TEST_CASE("進行状態: 別スレッドから段階を読み、request_stop で FlowStopped (発話は残る)") {
    w3l::TempFlowDb tmp;
    set_env(Json{{"DISCUTERE_FLOW_ROUNDS", "3"}, {"DISCUTERE_FLOW_TURNS_PER_ROUND", "3"}, {"DISCUTERE_FLOW_PERSONA_COUNT", "3"}});
    auto state = std::make_shared<FlowRunState>();
    std::atomic<int> persona_calls{0};
    w3l::FnLLM llm([&](const persona_engine::LLMInvokeArgs& a) {
        if (str::contains(a.prompt, "価値軸")) return persona_engine::LLMResult::success("[]");
        if (++persona_calls == 3) state->request_stop();  // 3 回目の呼び出しの最中に止める
        return persona_engine::LLMResult::success("発話");
    });
    FlowDirectorDeps deps;
    deps.sessionId = "stop-session";
    deps.rng = w3l::lcg(5);
    deps.log = [](const std::string&) {};
    deps.warn = [](const std::string&) {};
    deps.state = state;
    bool stopped = false;
    try {
        run_discussion_flow(llm, "停止テスト", {}, deps);
    } catch (const FlowStopped&) {
        stopped = true;
    }
    CHECK(stopped);
    const auto snap = state->snapshot();
    CHECK_EQ(snap.phase, std::string("stopped"));
    CHECK(snap.stopRequested);
    CHECK_EQ(snap.round, 1);
    CHECK_EQ(snap.rounds, 3);
    CHECK_EQ(snap.utterances, 3);
    auto db = flow::db::get_flow_db();
    CHECK_EQ(db->all("SELECT id FROM flow_utterance WHERE session_id = 'stop-session'").size(), 3u);
    // 結論は作らない (再議論で回し直せる状態のまま)。
    CHECK(!db->get("SELECT 1 FROM flow_conclusion WHERE session_id = 'stop-session'").has_value());

    // 止めなければ done まで進み、別スレッドから途中の段階を読める。
    auto state2 = std::make_shared<FlowRunState>();
    std::atomic<bool> seen_round{false};
    w3l::FnLLM llm2([&](const persona_engine::LLMInvokeArgs&) {
        if (state2->snapshot().phase == "round") seen_round = true;
        return persona_engine::LLMResult::success("[]");
    });
    deps.sessionId = "run-session";
    deps.state = state2;
    std::thread t([&] { run_discussion_flow(llm2, "進行テスト", {}, deps); });
    t.join();
    CHECK(seen_round.load());
    CHECK_EQ(state2->snapshot().phase, std::string("done"));
    set_env(Json::object());
}

TEST_CASE("conclusion: 止揚も世論も無ければ LLM を呼ばず (結論なし)、注記は併記される") {
    w3l::TempFlowDb tmp;
    w3l::FnLLM llm([](const persona_engine::LLMInvokeArgs&) { return persona_engine::LLMResult::success("{}"); });
    GenerateConclusionArgs a;
    a.theme = "テーマ";
    a.sessionId = "s-none";
    a.paperId = "p";
    a.annotation = "  議論適性: 低  ";
    a.warn = [](const std::string&) {};
    const auto r = generate_conclusion(llm, a);
    CHECK_EQ(llm.count, 0);
    CHECK(!r.concluded);
    CHECK_EQ(r.summary, std::string("(結論なし)\n\n※ 議論適性: 低"));
    auto db = flow::db::get_flow_db();
    const auto row = db->get("SELECT * FROM flow_conclusion WHERE session_id = 's-none'");
    CHECK(row.has_value());
    CHECK_EQ(row->str("aufhebung_json"), std::string("[]"));
    CHECK_EQ(row->i64("concluded"), 0);
    const auto cache = db->get("SELECT * FROM conclusion_cache WHERE gap_id = 'flow:s-none'");
    CHECK(cache.has_value());
    CHECK(cache->is_null("conclusion"));

    // LLM の応答 JSON の境界: 巨大な応答 (上限超過) と壊れた JSON は未検証の生テキストに落ちる。
    VoteResult vr;
    vr.winner = "u1";
    a.voteResults = {vr, vr};  // 重複 winner は 1 つにまとめる
    FlowUtteranceRecord u;
    u.id = "u1";
    u.personaName = "A";
    u.text = "意見";
    a.allUtterances = {u};
    a.annotation.reset();
    a.sessionId = "s-big";
    const std::string huge = "{\"summary\": \"" + std::string(5u << 20, 'x') + "\"}";
    w3l::FnLLM big([&](const persona_engine::LLMInvokeArgs&) { return persona_engine::LLMResult::success(huge); });
    const auto rb = generate_conclusion(big, a);
    CHECK_EQ(big.count, 2);
    CHECK(!rb.concluded);
    CHECK(str::starts_with(rb.summary, UNVERIFIED_PREFIX));
    CHECK_EQ(db->get("SELECT top_utterance_ids_json AS t FROM flow_conclusion WHERE session_id='s-big'")->str("t"), std::string("[\"u1\"]"));
    a.sessionId = "s-broken";
    w3l::FnLLM broken([](const persona_engine::LLMInvokeArgs&) { return persona_engine::LLMResult::success("{\"summary\": "); });
    CHECK(!generate_conclusion(broken, a).concluded);
    a.sessionId = "s-empty";
    w3l::FnLLM empty_summary([](const persona_engine::LLMInvokeArgs&) { return persona_engine::LLMResult::success("{\"summary\": \"   \"}"); });
    const auto re = generate_conclusion(empty_summary, a);
    CHECK(!re.concluded);
    a.sessionId = "s-ok";
    w3l::FnLLM good([](const persona_engine::LLMInvokeArgs&) { return persona_engine::LLMResult::success("{\"summary\": \" 結論 \"}"); });
    const auto rg = generate_conclusion(good, a);
    CHECK(rg.concluded);
    CHECK_EQ(rg.summary, std::string("【収束】 結論 "));
    CHECK_EQ(db->get("SELECT conclusion FROM conclusion_cache WHERE gap_id='flow:s-ok'")->str("conclusion"), std::string("結論"));
}

int main() { return di::test::run_all(); }
