// blackbox_engine のテスト (1/3)。準備は blackbox_engine_support.hpp。
#include "blackbox_engine_support.hpp"

using namespace blackbox_engine_support;

TEST_CASE("no rule -> LLM decision + candidate registered (does not fire)") {
    Fixture f;
    int calls = 0;
    auto r = f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
    CHECK_EQ(r.decision.source, std::string("llm"));
    CHECK_EQ(calls, 1);
    auto all = f.rules->list_by_domain("d");
    CHECK_EQ(all.size(), std::size_t(1));
    CHECK_EQ(all[0].state, std::string("candidate"));
    f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
    CHECK_EQ(calls, 2);
}

TEST_CASE("same proposal merges into proposals++") {
    Fixture f;
    int calls = 0;
    for (int i = 0; i < 3; ++i) f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
    auto all = f.rules->list_by_domain("d");
    CHECK_EQ(all.size(), std::size_t(1));
    CHECK_EQ(all[0].proposals, std::int64_t(3));
}

TEST_CASE("shadow agreements promote candidate -> trial and firing starts") {
    EngineOptions o;
    o.shadowPromote = 3;
    Fixture f(o);
    int calls = 0;
    for (int i = 0; i < 4; ++i) f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
    CHECK_EQ(f.rules->list_by_domain("d")[0].state, std::string("trial"));
    CHECK_EQ(calls, 4);
    auto r = f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
    CHECK_EQ(calls, 4);
    CHECK_EQ(r.decision.source, std::string("rule"));
    CHECK_EQ(r.decision.status, std::string("pending_review"));
}

TEST_CASE("shadow conflicts retire candidate") {
    EngineOptions o;
    o.shadowConflictLimit = 2;
    Fixture f(o);
    int calls = 0;
    f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
    auto dissent = rain_llm(calls, {Json{{"rain", false}}, true});
    f.engine.decide("d", Json::object(), RAINY, dissent);
    f.engine.decide("d", Json::object(), RAINY, dissent);
    CHECK_EQ(f.rules->list_by_domain("d")[0].state, std::string("retired"));
}

TEST_CASE("trial rule OK x3 -> auto, then decides immediately") {
    Fixture f;
    auto rule = f.engine.add_rule(rain_draft());
    CHECK_EQ(rule.state, std::string("trial"));
    int calls = 0;
    for (int i = 0; i < 3; ++i) {
        auto r = f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
        CHECK_EQ(r.decision.status, std::string("pending_review"));
        CHECK_EQ(r.decision.rationale, std::string("ルール「テスト」で先に判定しました — OK/NG をお願いします (承認 ") +
                                           std::to_string(i) + "/3)");
        f.engine.record_verdict(r.decisionId, "ok");
    }
    CHECK_EQ(f.rules->get(rule.id)->state, std::string("auto"));
    auto r = f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
    CHECK_EQ(r.decision.status, std::string("auto"));
    CHECK_EQ(r.decision.rationale, std::string("ルール「テスト」で判定 (peakAgreement >= 0.6)"));
    CHECK_EQ(calls, 0);
    CHECK(f.ledger->list_pending(std::string("d")).empty());
}

DI_TEST_MAIN()
