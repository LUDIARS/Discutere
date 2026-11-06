// llm_gateway のテスト (4/4)。準備は llm_gateway_support.hpp。
#include "llm_gateway_support.hpp"

using namespace llm_gateway_support;

TEST_CASE("rollingSummary keepRecent 0 folds everything and custom estimator is used") {
    auto turns = make_turns(5);
    std::size_t summarized = 0;
    auto res = rolling_summary(turns, [&](const std::vector<HistoryTurn>& older) {
        summarized = older.size();
        return std::string("S");
    }, {4, 0, [](std::string_view) { return 1.0; }});
    CHECK(res.compacted);
    CHECK_EQ(summarized, std::size_t(5));
    CHECK(res.recent.empty());
    // 推定 1 × 5 = 5 > 4 なので畳む。予算ちょうど (5) なら畳まない。
    auto same = rolling_summary(turns, [](const auto&) { return std::string("S"); }, {5, 0, [](std::string_view) { return 1.0; }});
    CHECK(!same.compacted);
}

// ── pickTier ──
TEST_CASE("pickTier honors forceTier / strong kinds / simple kinds / large input / custom rules") {
    CHECK_EQ(pick_tier({"utterance", std::nullopt, std::string("strong")}), std::string("strong"));
    CHECK_EQ(pick_tier({"converge", std::nullopt, std::nullopt}), std::string("strong"));
    CHECK_EQ(pick_tier({"utterance", std::nullopt, std::nullopt}), std::string("cheap"));
    CHECK_EQ(pick_tier({"classify", std::nullopt, std::nullopt}), std::string("cheap"));
    CHECK_EQ(pick_tier({"utterance", 9000.0, std::nullopt}), std::string("strong"));
    CHECK_EQ(pick_tier({"review", std::nullopt, std::nullopt}, RoutingRules{std::vector<std::string>{"review"}, std::nullopt}),
             std::string("strong"));
    CHECK_EQ(pick_tier({"utterance", 3000.0, std::nullopt}, RoutingRules{std::nullopt, 2000.0}), std::string("strong"));
}

TEST_CASE("pickTier matches Node golden (threshold exact / +1)") {
    for (const auto& c : load("/llm_gateway/cases.json")["tiers"]) {
        RoutingInput in;
        in.kind = c["input"]["kind"].get<std::string>();
        if (c["input"].contains("inputTokens")) in.inputTokens = c["input"]["inputTokens"].get<double>();
        if (c["input"].contains("forceTier")) in.forceTier = c["input"]["forceTier"].get<std::string>();
        std::optional<RoutingRules> rules;
        if (c.contains("rules")) {
            RoutingRules r;
            if (c["rules"].contains("strongKinds")) r.strongKinds = c["rules"]["strongKinds"].get<std::vector<std::string>>();
            if (c["rules"].contains("strongAboveTokens")) r.strongAboveTokens = c["rules"]["strongAboveTokens"].get<double>();
            rules = r;
        }
        CHECK_EQ(pick_tier(in, rules), c["tier"].get<std::string>());
    }
}

TEST_CASE("estimateTokens handles huge and invalid UTF-8 input linearly") {
    const std::string huge(4u << 20, 'a');
    CHECK_EQ(estimate_tokens(huge), static_cast<long long>((4u << 20) / 4));
    // 不正なバイトは U+FFFD (非 CJK) として 1 文字に数える。
    CHECK_EQ(estimate_tokens(std::string("\xff\xfe\xfd\xfc", 4)), 1LL);
}

DI_TEST_MAIN()
