// w3m_facilitator のテスト (3/4)。準備は w3m_facilitator_support.hpp。
#include "w3m_facilitator_support.hpp"

using namespace w3m_facilitator_support;

TEST_CASE("expand: 壊れた JSON・opening 無し・LLM 失敗では persona を足さない (巨大応答も上限で拒否)") {
    for (const std::string& text : {std::string("not json"), std::string(R"({"name":"x"})"), std::string(R"({"opening": 5})"),
                                    "{\"opening\":\"" + std::string(2u << 20, 'a') + "\"}"}) {
        World w;
        const auto c = w.discussion("拡張テスト 壊れ", {"persona:a"}, "discord:1/70");
        auto llm = std::make_shared<MockLLMClient>(std::vector<MockResponder>{
            [](const LLMInvokeArgs&) -> Variant { return std::string(R"({"found": false})"); },
            [text](const LLMInvokeArgs&) -> Variant { return text; }});
        pf::create_facilitator(w.deps(llm))->tick_once();
        CHECK_EQ(w.provider->posts.load(), 0);
        CHECK_EQ(w.gap_status(c.gapId), std::string(""));
    }
}

TEST_CASE("start/stop: 定期 tick が background で走り、stop 後は走らない・二重 start は無視") {
    World w;
    const auto c = w.discussion("拡張テスト 定期", {"persona:a"}, "discord:1/80");
    auto d = w.deps(scripted_llm());
    d.options.tickMs = 10;
    auto fac = pf::create_facilitator(d);
    fac->start();
    fac->start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (w.provider->posts.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(w.provider->posts.load() >= 1);
    fac->stop();
    const int after = w.provider->posts.load();
    w.say(c.sessionId, "persona:z", "再開");  // 新発言 → 次 tick で再 arm されうるが stop 済みなので走らない
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(w.provider->posts.load(), after);
}

// ─── JS の値変換 (golden) ────────────────────────────────────────

TEST_CASE("js_value: Number(x) / String(x) が Node と一致 (golden)") {
    for (const auto& c : test::load_fixture("w3m/js_values.json")) {
        const Json& v = c["v"];
        const double n = pf::js_number_of(&v);
        if (c["num"] == "NaN") {
            CHECK(std::isnan(n));
        } else {
            CHECK_EQ(n, c["num"].get<double>());
        }
        CHECK_EQ(pf::js_string(v), c["str"].get<std::string>());
    }
    CHECK(std::isnan(pf::js_number_of(nullptr)));  // undefined
}

DI_TEST_MAIN()
