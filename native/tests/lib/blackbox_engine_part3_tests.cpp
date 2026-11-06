// blackbox_engine のテスト (3/3)。準備は blackbox_engine_support.hpp。
#include "blackbox_engine_support.hpp"

using namespace blackbox_engine_support;

TEST_CASE("shadow evaluation does not move when condition does not hold") {
    Fixture f;
    int calls = 0;
    f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
    f.engine.decide("d", Json::object(), DRY, rain_llm(calls, {Json{{"rain", false}}, true}));
    auto rule = f.rules->list_by_domain("d")[0];
    CHECK_EQ(rule.shadowAgreements, std::int64_t(0));
    CHECK_EQ(rule.shadowConflicts, std::int64_t(0));
}

TEST_CASE("live rules are tried in priority order and stats count decisions") {
    auto bb = make_memory_blackbox();
    auto low = rain_draft();
    low.output = Json{{"rain", "low"}};
    low.priority = 1;
    auto high = rain_draft();
    high.output = Json{{"rain", "high"}};
    high.priority = 5;
    bb.engine->add_rule(low);
    bb.engine->add_rule(high);
    int calls = 0;
    auto r = bb.engine->decide("d", Json::object(), RAINY, rain_llm(calls));
    CHECK_EQ(json_stringify(r.decision.output), std::string(R"({"rain":"high"})"));
    bb.engine->decide("d", Json::object(), DRY, rain_llm(calls));
    auto s = bb.stats("d");
    CHECK_EQ(s.window, std::int64_t(2));
    CHECK_EQ(s.ruleDecisions, std::int64_t(1));
    CHECK_EQ(s.llmDecisions, std::int64_t(1));
    CHECK_EQ(s.ruleCoverage, 0.5);
    CHECK_EQ(json_stringify(to_json(s)),
             std::string(R"({"domain":"d","window":2,"ruleDecisions":1,"llmDecisions":1,"ruleCoverage":0.5,"pendingReview":2,"ruleStates":{"candidate":1,"trial":2,"auto":0,"retired":0}})"));
}

TEST_CASE("memory ledger list_pending/list_recent limit slicing matches JS slice") {
    MemoryDecisionLedger ledger;
    for (int i = 0; i < 5; ++i) {
        DecisionInput in;
        in.domain = "d";
        in.source = "llm";
        in.status = "pending_review";
        in.createdAt = "t";
        ledger.record(in);
    }
    auto two = ledger.list_recent("d", 2);
    CHECK_EQ(two.size(), std::size_t(2));
    CHECK_EQ(two[0].id, std::int64_t(5));
    CHECK_EQ(ledger.list_recent("d", 0).size(), std::size_t(5));     // slice(-0) は全件
    CHECK_EQ(ledger.list_recent("d", 100).size(), std::size_t(5));
    CHECK_EQ(ledger.list_pending(std::nullopt, 3).size(), std::size_t(3));
}

DI_TEST_MAIN()
