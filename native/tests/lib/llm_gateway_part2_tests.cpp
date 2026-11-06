// llm_gateway のテスト (2/4)。準備は llm_gateway_support.hpp。
#include "llm_gateway_support.hpp"

using namespace llm_gateway_support;

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

DI_TEST_MAIN()
