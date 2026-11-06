// llm_gateway のテスト (1/4)。準備は llm_gateway_support.hpp。
#include "llm_gateway_support.hpp"

using namespace llm_gateway_support;

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

DI_TEST_MAIN()
