// w5u_discord_live のテスト (1/4)。準備は w5u_discord_live_support.hpp。
#include "w5u_discord_live_support.hpp"

using namespace w5u_discord_live_support;

TEST_CASE("golden: 学習フローは Core 未設定なら起動できない旨を出す (Node と同じ本文)") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    Harness h;
    const auto fx = w5u::load_fixture("discord_live.json");
    start_forum_flow(input("TL", "learning"), h.deps(env.games()));
    CHECK(h.discord.post_contents("TL") == contents(fx["learningNoCore"]));
}

TEST_CASE("golden: レビューゲート — 準備・草案・声の準備・案内・戻す・相づち・調整・受付確認・開始 (Node と同じ投稿列)") {
    w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_ENABLED", "true"}});
    reset_flow_live_for_test();
    Harness h;
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion();
    const auto fx = w5u::load_fixture("discord_live.json")["review"];
    start_forum_flow(input("TR", "discussion", {"機密"}), deps);
    std::vector<std::size_t> marks{h.discord.post_count()};
    for (const char* text : {"戻す", "はい", "メカニクスにガチャを追加", "受付確認"}) {
        CHECK(handle_paper_review_reply("TR", "G1", text, deps));
        marks.push_back(h.discord.post_count());
    }
    CHECK_EQ(has_paper_review("TR"), fx["hadReview"].get<bool>());
    CHECK(handle_paper_review_reply("TR", "G1", "開始", deps));
    CHECK(w5u::drain(h.ex));
    const auto expected = contents(fx["posts"]);
    auto actual = h.discord.post_contents("TR");
    CHECK(actual.size() >= expected.size());
    actual.resize(expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) CHECK_EQ(actual[i], expected[i]);
    for (std::size_t i = 0; i < marks.size(); ++i) CHECK_EQ(marks[i], fx["marks"][i].get<std::size_t>());
    // 承認後は議論 (偽) が走り、ペーパーは started になってレビュー待ちではなくなる。
    CHECK_EQ(h.dispatched.size(), std::size_t{1});
    CHECK(h.dispatched_deps[0].paperOverride.has_value());
    CHECK_EQ(h.dispatched_deps[0].sessionId.value_or(""), forum_session_id("TR"));
    CHECK(!has_paper_review("TR"));
    CHECK(!is_discussion_running("TR"));
    // approval_guide は既定 (タイムアウトなし) で Node と同じ。
    CHECK_EQ(approval_guide(), w5u::load_fixture("discord_live.json")["approvalGuideDefault"].get<std::string>());
}

TEST_CASE("approval_guide: timeoutMs があれば分 / 秒で併記する (Math.round)") {
    {
        w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_TIMEOUT_MS", "120000"}});
        CHECK(str::ends_with(approval_guide(), "\n(2 分無操作なら草案のまま自動で始めます。ただし外部の声の選択待ち・受付待ちでは自動開始しません)"));
    }
    {
        w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_TIMEOUT_MS", "29000"}});
        CHECK(str::ends_with(approval_guide(), "\n(29 秒無操作なら草案のまま自動で始めます。ただし外部の声の選択待ち・受付待ちでは自動開始しません)"));
    }
    {
        w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_TIMEOUT_MS", "90000"}});
        CHECK(approval_guide().find("(2 分無操作") != std::string::npos);  // 1.5 → 2 (Math.round)
    }
}

DI_TEST_MAIN()
