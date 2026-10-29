// @ludiars/blackbox engine.test.ts の移植 (学習ループ)。
#include "check.hpp"

#include "lib/blackbox/blackbox.hpp"

using namespace di;
using namespace di::lib::blackbox;

namespace {
const Json COND_RAIN = Json{{"op", "cmp"}, {"feature", "peakAgreement"}, {"cmp", ">="}, {"value", 0.6}};
const Json RAINY = Json{{"peakAgreement", 0.8}};
const Json DRY = Json{{"peakAgreement", 0.1}};

struct Fixture {
    std::shared_ptr<MemoryRuleStore> rules = std::make_shared<MemoryRuleStore>();
    std::shared_ptr<MemoryDecisionLedger> ledger = std::make_shared<MemoryDecisionLedger>();
    BlackBoxEngine engine;
    explicit Fixture(const EngineOptions& opts = {}) : engine(rules, ledger, opts) {}
};

struct Overrides {
    std::optional<Json> output;
    bool dropProposal = false;
};

/** proposedRule 付きで {rain:true} を返す LLM フォールバック。呼び出し回数を数える。 */
LlmFallback rain_llm(int& calls, Overrides o = {}) {
    return [&calls, o](const Json&, const Json&, const LlmContext&) {
        calls += 1;
        LlmJudgement j;
        j.output = o.output ? *o.output : Json{{"rain", true}};
        j.confidence = 0.9;
        j.rationale = "テスト用判定";
        if (!o.dropProposal) j.proposedRule = RuleProposal{"一致率 0.6 以上は雨", COND_RAIN, Json{{"rain", true}}, 0.8, std::nullopt};
        return j;
    };
}

RuleDraft rain_draft(const std::string& domain = "d") {
    RuleDraft d;
    d.domain = domain;
    d.description = "テスト";
    d.when = COND_RAIN;
    d.output = Json{{"rain", true}};
    return d;
}
} // namespace

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
