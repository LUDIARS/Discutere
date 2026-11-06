// llm_gateway のテスト (3/4)。準備は llm_gateway_support.hpp。
#include "llm_gateway_support.hpp"

using namespace llm_gateway_support;

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

DI_TEST_MAIN()
