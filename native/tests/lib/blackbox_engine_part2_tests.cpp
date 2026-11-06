// blackbox_engine のテスト (2/3)。準備は blackbox_engine_support.hpp。
#include "blackbox_engine_support.hpp"

using namespace blackbox_engine_support;

TEST_CASE("trial rule NG x3 -> retired and same fingerprint re-proposal is blocked") {
    Fixture f;
    auto rule = f.engine.add_rule(rain_draft());
    int calls = 0;
    for (int i = 0; i < 3; ++i) {
        auto r = f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
        f.engine.record_verdict(r.decisionId, "ng");
    }
    CHECK_EQ(f.rules->get(rule.id)->state, std::string("retired"));
    std::size_t seen_retired = 0;
    std::string seen_text;
    LlmFallback llm = [&](const Json&, const Json&, const LlmContext& ctx) {
        seen_retired = ctx.retiredRules.size();
        if (!ctx.retiredRules.empty()) seen_text = ctx.retiredRules[0].whenText;
        calls += 1;
        LlmJudgement j;
        j.output = Json{{"rain", true}};
        j.confidence = 0.9;
        j.rationale = "t";
        j.proposedRule = RuleProposal{"再提案", COND_RAIN, Json{{"rain", true}}, std::nullopt, std::nullopt};
        return j;
    };
    f.engine.decide("d", Json::object(), RAINY, llm);
    CHECK_EQ(seen_retired, std::size_t(1));
    CHECK_EQ(seen_text, std::string("peakAgreement >= 0.6"));
    CHECK_EQ(f.rules->list_by_domain("d").size(), std::size_t(1));
}

TEST_CASE("NG on an LLM decision flips the shadow evaluation") {
    Fixture f;
    int calls = 0;
    f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
    auto r = f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
    CHECK_EQ(f.rules->list_by_domain("d")[0].shadowAgreements, std::int64_t(1));
    f.engine.record_verdict(r.decisionId, "ng");
    auto rule = f.rules->list_by_domain("d")[0];
    CHECK_EQ(rule.shadowAgreements, std::int64_t(0));
    CHECK_EQ(rule.shadowConflicts, std::int64_t(1));
    CHECK_EQ(*f.ledger->get(r.decisionId)->verdict, std::string("ng"));
}

TEST_CASE("LLM decisions are pending_review by default") {
    Fixture f;
    int calls = 0;
    auto r = f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
    CHECK_EQ(r.decision.rationale, std::string("LLM が判定: テスト用判定"));
    CHECK_EQ(f.ledger->list_pending(std::string("d")).size(), std::size_t(1));
}

TEST_CASE("reviewLlmDecisions false keeps old behavior (LLM decision auto)") {
    EngineOptions o;
    o.reviewLlmDecisions = false;
    Fixture f(o);
    int calls = 0;
    auto r = f.engine.decide("d", Json::object(), RAINY, rain_llm(calls));
    CHECK_EQ(r.decision.status, std::string("auto"));
    CHECK(f.ledger->list_pending(std::string("d")).empty());
}

TEST_CASE("addRule keeps manual->trial / seed->auto defaults") {
    Fixture f;
    auto manual = rain_draft();
    manual.source = "manual";
    CHECK_EQ(f.engine.add_rule(manual).state, std::string("trial"));
    auto seed = rain_draft("d2");
    seed.source = "seed";
    CHECK_EQ(f.engine.add_rule(seed).state, std::string("auto"));
}

DI_TEST_MAIN()
