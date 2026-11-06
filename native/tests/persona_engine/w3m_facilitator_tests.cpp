// w3m_facilitator のテスト (1/4)。準備は w3m_facilitator_support.hpp。
#include "w3m_facilitator_support.hpp"

using namespace w3m_facilitator_support;

TEST_CASE("evaluate: 新発言で active + 再 arm / idle + armed で intervene / 未達・disarm は wait") {
    auto r = pf::evaluate({5, 3, 0, 9999}, {4, false}, 1000);
    CHECK_EQ(r.mode, std::string("active"));
    CHECK(r.state == (pf::SessionState{5, true}));
    r = pf::evaluate({5, 3, 0, 5000}, {5, true}, 1000);
    CHECK_EQ(r.mode, std::string("intervene"));
    CHECK(r.state == (pf::SessionState{5, false}));
    CHECK_EQ(pf::evaluate({5, 3, 0, 5000}, {5, false}, 1000).mode, std::string("wait"));
    CHECK_EQ(pf::evaluate({5, 3, 4900, 5000}, {5, true}, 1000).mode, std::string("wait"));
    // 境界: idle がちょうど idleGapMs なら介入する (>=)、1 ms 足りなければ待つ
    CHECK_EQ(pf::evaluate({5, 3, 4000, 5000}, {5, true}, 1000).mode, std::string("intervene"));
    CHECK_EQ(pf::evaluate({5, 3, 4001, 5000}, {5, true}, 1000).mode, std::string("wait"));
}

TEST_CASE("tick_once: 止揚が無い議論は新 persona で広げ、止揚到達の議論は収束して gap closed・session ended") {
    World w;
    const auto expand_case = w.discussion("拡張テスト", {"persona:advocate"}, "discord:1/10");
    const auto converge_case = w.discussion("収束テスト", {"persona:advocate"}, "discord:1/20");
    std::vector<pf::FacilitatorConvergedEvent> events;
    auto d = w.deps(scripted_llm());
    d.onConverged = [&](const pf::FacilitatorConvergedEvent& e) { events.push_back(e); };
    auto fac = pf::create_facilitator(d);
    // 進行役 persona は構築時に登録される
    CHECK(w.personas->get("facilitator").has_value());
    CHECK_EQ(w.personas->get("facilitator")->display_name, std::string("司会 結"));

    fac->tick_once();

    const auto expanded = w.core->raw().all("SELECT raw_content FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:dyn-%'",
                                            {expand_case.sessionId});
    CHECK_EQ(expanded.size(), std::size_t(1));
    CHECK_EQ(expanded[0].str("raw_content"), std::string("本当にそれは時間に見合う?"));
    // 動的 persona が登録される (name/display_name/traits は LLM の JSON から)
    const auto dyn = w.core->raw().get("SELECT speaker_id FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:dyn-%'",
                                       {expand_case.sessionId});
    const auto pid = dyn->str("speaker_id").substr(8);
    CHECK_EQ(str::js_length(pid), std::size_t(12));  // "dyn-" + 8
    CHECK_EQ(w.personas->get(pid)->display_name, std::string("費田 渋"));
    CHECK_EQ(w.personas->get(pid)->traits, std::string(R"(["慎重"])"));

    const auto stock = w.core->raw().all("SELECT summary FROM aufhebung_stock WHERE gap_id = ?", {converge_case.gapId});
    CHECK_EQ(stock.size(), std::size_t(1));
    CHECK(str::contains(stock[0].str("summary"), "統合"));
    const auto conv = w.core->raw().get("SELECT raw_content FROM utterances WHERE session_id = ? AND speaker_id = 'persona:facilitator'",
                                        {converge_case.sessionId});
    CHECK(conv && str::contains(conv->str("raw_content"), "【収束】"));
    CHECK_EQ(conv->str("raw_content"), std::string("【収束】\n止揚Xを軸にした結論。"));
    CHECK_EQ(w.gap_status(converge_case.gapId), std::string("closed"));
    CHECK(!w.core->raw().get("SELECT ended_at FROM sessions WHERE id = ?", {converge_case.sessionId})->is_null("ended_at"));
    CHECK(w.core->raw().get("SELECT ended_at FROM sessions WHERE id = ?", {expand_case.sessionId})->is_null("ended_at"));

    CHECK_EQ(events.size(), std::size_t(1));
    CHECK_EQ(events[0].gapId, converge_case.gapId);
    CHECK_EQ(events[0].scene.value_or(""), std::string("discord:1/20"));
    CHECK_EQ(events[0].title, std::string("収束テスト"));
    CHECK_EQ(events[0].summary, std::string("止揚Xを軸にした結論。"));

    // 2 回目の tick: 収束済み (closed) は対象外、拡張済みは新発言で active になるだけ (再介入しない)
    const int before = w.provider->posts.load();
    fac->tick_once();
    CHECK_EQ(w.provider->posts.load(), before);
}

TEST_CASE("tick_once: headless (scene=gap:) も駆動し、scene の無い/他の scene・別 workspace・dismissed は対象外") {
    World w;
    const auto headless = w.discussion("拡張テスト headless", {"persona:a"}, "gap:x");
    const auto web = w.discussion("拡張テスト web", {"persona:a"}, "web:room");
    const auto dismissed = w.discussion("拡張テスト dismissed", {"persona:a"}, "discord:1/30");
    w.core->raw().run("UPDATE design_gaps SET status = 'dismissed' WHERE id = ?", {dismissed.gapId});
    auto fac = pf::create_facilitator(w.deps(scripted_llm()));
    fac->tick_once();
    auto count = [&](const std::string& s) {
        return w.core->raw().get("SELECT COUNT(*) AS n FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:dyn-%'", {s})->i64("n");
    };
    CHECK_EQ(count(headless.sessionId), 1);
    CHECK_EQ(count(web.sessionId), 0);
    CHECK_EQ(count(dismissed.sessionId), 0);
}

DI_TEST_MAIN()
