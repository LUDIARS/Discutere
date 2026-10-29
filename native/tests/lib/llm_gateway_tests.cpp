// @ludiars/llm-gateway の移植 (lib/lapilli/packages/llm-gateway/src/index.test.ts) +
// golden (native/tests/golden/w1-B-llm_gateway.gen.ts が記録した Node の出力)。
#include "check.hpp"

#include <cmath>

#include "base/fsutil.hpp"
#include "lib/llm_gateway/llm_gateway.hpp"

using namespace di;
using namespace di::lib::llm_gateway;

namespace {
Json load(const std::string& rel) {
    auto text = fs::read_file(test::fixtures_dir() + rel);
    if (!text) throw std::runtime_error("fixture missing: " + rel);
    return parse_json_or_throw(*text, {8u << 20, 1'000'000, 64});
}
std::vector<HistoryTurn> make_turns(int n) {
    std::vector<HistoryTurn> t;
    for (int i = 0; i < n; ++i) t.push_back({"user", "turn " + std::to_string(i) + " " + std::string(40, 'x')});
    return t;
}
} // namespace

// ── estimateTokens ──
TEST_CASE("estimateTokens returns 0 for empty") { CHECK_EQ(estimate_tokens(""), 0LL); }

TEST_CASE("estimateTokens counts CJK denser than latin") {
    const auto jp = estimate_tokens("日本語のテキスト");
    const auto en = estimate_tokens("english text here");
    CHECK(jp > 0);
    CHECK(en > 0);
    CHECK_EQ(jp, static_cast<long long>(std::ceil(8 / 1.5)));
}

TEST_CASE("estimateTokens matches Node golden") {
    for (const auto& c : load("/llm_gateway/cases.json")["tokens"]) {
        CHECK_EQ(estimate_tokens(c["text"].get<std::string>()), c["tokens"].get<long long>());
    }
}

// ── orderSegments ──
TEST_CASE("orderSegments orders fixed before semi before volatile") {
    auto r = order_segments({{"volatile", "user", "history tail"}, {"fixed", "system", "persona"}, {"semi", "user", "attached doc"}});
    CHECK_EQ(r.messages[0].content, std::string("persona"));
    CHECK_EQ(r.messages[0].role, std::string("system"));
    CHECK_EQ(r.cacheBreakpoint, std::size_t(2));
    CHECK_EQ(r.messages.back().content, std::string("history tail"));
}

TEST_CASE("orderSegments merges adjacent same-role same-stability segments") {
    auto r = order_segments({{"fixed", "system", "rule A"}, {"fixed", "system", "rule B"}});
    CHECK_EQ(r.messages.size(), std::size_t(1));
    CHECK_EQ(r.messages[0].content, std::string("rule A\n\nrule B"));
    CHECK_EQ(r.cacheBreakpoint, std::size_t(1));
}

TEST_CASE("orderSegments does not merge across stability boundary") {
    auto r = order_segments({{"semi", "user", "doc"}, {"volatile", "user", "now"}});
    CHECK_EQ(r.messages.size(), std::size_t(2));
    CHECK_EQ(r.cacheBreakpoint, std::size_t(1));
}

TEST_CASE("orderSegments drops empty segments") {
    auto r = order_segments({{"fixed", "system", "   "}, {"volatile", "user", "hi"}});
    CHECK_EQ(r.messages.size(), std::size_t(1));
    CHECK_EQ(r.cacheBreakpoint, std::size_t(0));
}

TEST_CASE("orderSegments keeps insertion order within same stability") {
    auto r = order_segments({{"fixed", "system", "first"}, {"fixed", "assistant", "second"}});
    CHECK_EQ(r.messages[0].content, std::string("first"));
    CHECK_EQ(r.messages[1].content, std::string("second"));
}

TEST_CASE("orderSegments matches Node golden") {
    for (const auto& c : load("/llm_gateway/cases.json")["ordering"]) {
        std::vector<Segment> segs;
        for (const auto& s : c["segments"])
            segs.push_back({s["stability"].get<std::string>(), s["role"].get<std::string>(), s["text"].get<std::string>()});
        auto r = order_segments(segs);
        Json messages = Json::array();
        for (const auto& m : r.messages) messages.push_back(to_json(m));
        CHECK_EQ(json_stringify(messages), json_stringify(c["result"]["messages"]));
        CHECK_EQ(static_cast<long long>(r.cacheBreakpoint), c["result"]["cacheBreakpoint"].get<long long>());
    }
}

// ── rollingSummary ──
TEST_CASE("rollingSummary passes through when under budget") {
    auto turns = make_turns(10);
    auto res = rolling_summary(turns, [](const auto&) { return std::string("SUM"); }, {1'000'000, 4, {}});
    CHECK(!res.compacted);
    CHECK(!res.summary);
    CHECK_EQ(res.recent.size(), std::size_t(10));
}

TEST_CASE("rollingSummary summarizes older turns when over budget") {
    auto turns = make_turns(10);
    std::size_t summarized = 0;
    auto res = rolling_summary(turns, [&](const std::vector<HistoryTurn>& older) {
        summarized = older.size();
        return std::string("SUMMARY");
    }, {1, 4, {}});
    CHECK(res.compacted);
    CHECK_EQ(*res.summary, std::string("SUMMARY"));
    CHECK_EQ(res.recent.size(), std::size_t(4));
    CHECK_EQ(summarized, std::size_t(6));
    CHECK(res.recent.front() == turns[6]);
}

TEST_CASE("rollingSummary does not summarize when history is within keepRecent") {
    auto turns = make_turns(3);
    auto res = rolling_summary(turns, [](const auto&) { return std::string("SUM"); }, {1, 4, {}});
    CHECK(!res.compacted);
    CHECK(!res.summary);
    CHECK_EQ(res.recent.size(), std::size_t(3));
}

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
