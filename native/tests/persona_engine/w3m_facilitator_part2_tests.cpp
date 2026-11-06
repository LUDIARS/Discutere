// w3m_facilitator のテスト (2/4)。準備は w3m_facilitator_support.hpp。
#include "w3m_facilitator_support.hpp"

using namespace w3m_facilitator_support;

TEST_CASE("intervene: maxPersonas 超過で収束、aufhebung-only ポリシーでは persona 数で収束しない (ライブ tuning)") {
    World w;
    const auto many = w.discussion("拡張テスト 多人数", {"persona:a", "persona:b", "persona:c"}, "discord:1/40");
    runtime_settings::FacilitatorTuning t{30'000, 1000, 2, 9, "aufhebung-only"};
    auto d = w.deps(scripted_llm());
    d.tuning = [&] { return t; };
    auto fac = pf::create_facilitator(d);
    fac->tick_once();
    CHECK_EQ(w.gap_status(many.gapId), std::string(""));  // 拡張しただけ

    World w2;
    const auto many2 = w2.discussion("拡張テスト 多人数", {"persona:a", "persona:b", "persona:c"}, "discord:1/40");
    t.convergePolicy = "default";
    auto d2 = w2.deps(scripted_llm());
    d2.tuning = [&] { return t; };
    pf::create_facilitator(d2)->tick_once();
    CHECK_EQ(w2.gap_status(many2.gapId), std::string("closed"));  // 3 > maxPersonas(2)
}

TEST_CASE("prompt: 調整指示は topic に注入され、除外発話は載らず、人間の意見は下駄で上位に来る") {
    World w;
    const auto c = w.discussion("収束テスト 指示", {"persona:a", "user-123"}, "discord:1/50");
    discord_hook::add_facilitator_directive(w.core->raw(), {WS, c.gapId, "もっと簡単な言葉で", std::string("author-9")});
    const auto noisy = w.say(c.sessionId, "persona:b", "ノイズ発言ZZZ");
    core::noise::exclude_utterance(w.core->raw(), noisy, std::string("noise"));
    std::vector<std::string> prompts;
    std::mutex mu;
    auto fac = pf::create_facilitator(w.deps(scripted_llm(&prompts, &mu)));
    fac->tick_once();
    CHECK_EQ(w.gap_status(c.gapId), std::string("closed"));
    bool directive = false, author = false, noise = false, human_top = false;
    for (const auto& p : prompts) {
        directive = directive || str::contains(p, "もっと簡単な言葉で");
        author = author || str::contains(p, "author-9");
        noise = noise || str::contains(p, "ZZZ");
        human_top = human_top || str::contains(p, "人間「発言1」(+5 / 人間)");
    }
    CHECK(directive);
    CHECK(!author);  // author_id は prompt に出さない
    CHECK(!noise);
    CHECK(human_top);
}

TEST_CASE("converge_now: gap 無し・既に closed・LLM 失敗は ok=false、成功は ok=true、フック例外は握る") {
    World w;
    const auto c = w.discussion("任意の議題", {"persona:a"}, "discord:1/60");
    auto failing = std::make_shared<MockLLMClient>(std::vector<MockResponder>{
        [](const LLMInvokeArgs&) -> Variant { return LLMResult::failure("backend down"); }});
    auto fac = pf::create_facilitator(w.deps(failing));
    auto r = fac->converge_now(c.sessionId, "no-such-gap");
    CHECK(!r.ok);
    CHECK_EQ(r.error.value_or(""), std::string("gap not found"));
    r = fac->converge_now(c.sessionId, c.gapId);
    CHECK(!r.ok);
    CHECK_EQ(r.error.value_or(""), std::string("まとめ生成に失敗しました (LLM 応答なし)。再試行してください"));
    CHECK(w.logger->has("facilitator converge llm failed"));

    // JSON でない応答はそのまま本文にする
    auto plain = std::make_shared<MockLLMClient>(std::vector<MockResponder>{[](const LLMInvokeArgs&) -> Variant { return std::string("素のまとめ"); }});
    auto d = w.deps(plain);
    d.onConverged = [](const pf::FacilitatorConvergedEvent&) { throw std::runtime_error("hook boom"); };
    auto fac2 = pf::create_facilitator(d);
    r = fac2->converge_now(c.sessionId, c.gapId);
    CHECK(r.ok);
    CHECK(w.logger->has("facilitator onConverged hook failed"));
    CHECK_EQ(w.core->raw().get("SELECT raw_content FROM utterances WHERE session_id = ? AND speaker_id = 'persona:facilitator'", {c.sessionId})->str("raw_content"),
             std::string("【収束】\n素のまとめ"));
    r = fac2->converge_now(c.sessionId, c.gapId);
    CHECK(!r.ok);
    CHECK_EQ(r.error.value_or(""), std::string("already closed"));
}

DI_TEST_MAIN()
