// w3l_director のテスト (1/2)。準備は w3l_director_support.hpp。
#include "w3l_director_support.hpp"

using namespace w3l_director_support;

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

int main() { return di::test::run_all(); }
