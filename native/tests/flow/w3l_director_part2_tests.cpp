// w3l_director のテスト (2/2)。準備は w3l_director_support.hpp。
#include "w3l_director_support.hpp"

using namespace w3l_director_support;

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
