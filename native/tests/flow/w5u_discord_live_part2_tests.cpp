// w5u_discord_live のテスト (2/4)。準備は w5u_discord_live_support.hpp。
#include "w5u_discord_live_support.hpp"

using namespace w5u_discord_live_support;

TEST_CASE("議論 (レビューなし): 発話を persona 名で webhook 投稿・投票の 🏆/👍・集計・結論・onConcluded") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    Harness h;
    h.discord.add_thread("TD");
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion();
    deps.listExternalVoices = [](const std::vector<std::string>&, int) { return std::vector<ContextVoice>{ContextVoice{"声", "steam", {}}}; };
    start_forum_flow(input("TD", "discussion"), deps, h.hooks());
    const auto posts = h.discord.post_contents("TD");
    CHECK_EQ(posts.size(), std::size_t{3});
    CHECK_EQ(posts[0], std::string("🗣️ **企画/議論**のを始めます: 「周回の面白さ」"));
    CHECK(str::starts_with(posts[1], "**🗳️ ラウンド 1 投票結果**\n"));
    CHECK_EQ(posts[2], std::string("✅ **結論**\n周回は短くする"));
    const auto hooks = h.discord.snapshot(h.discord.webhooks);
    CHECK_EQ(hooks.size(), std::size_t{2});
    CHECK_EQ(hooks[0]["username"].get<std::string>(), compose_display_name(DisplayNameInput{"相馬", std::string("pro"), std::string("debater"), {}}));
    CHECK_EQ(hooks[0]["content"].get<std::string>(), std::string("それは違う。\n別の見方もある！"));  // humanize
    CHECK_EQ(hooks[0]["thread_id"].get<std::string>(), std::string("TD"));                      // 親の webhook + thread_id
    const auto reactions = h.discord.snapshot(h.discord.reactions);
    CHECK_EQ(reactions.size(), std::size_t{2});
    CHECK(str::starts_with(reactions[0], "TD/w2/"));  // winner (最初の発話 = 2 番目のメッセージ) に 🏆
    CHECK(str::starts_with(reactions[1], "TD/w3/"));  // 得票ありに 👍
    CHECK(str::ends_with(reactions[0], "/" + std::string("%F0%9F%8F%86")) || str::ends_with(reactions[0], "/🏆"));
    CHECK_EQ(h.concluded.size(), std::size_t{1});
    CHECK_EQ(h.concluded[0], std::string("discord:G1/TD|周回の面白さ|周回は短くする"));
    CHECK(!is_discussion_running("TD"));
    // bot の投稿は Authorization: Bot <token> (token はログに出さない)
    for (const auto& a : h.discord.snapshot(h.discord.auth_headers)) CHECK_EQ(a, std::string("Bot fake-bot-token-not-real"));
}

TEST_CASE("議論 (レビューなし): 声が引けないときは草案レビューに回る・未収束は (明確な結論には至りませんでした)") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    Harness h;
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion(false);
    start_forum_flow(input("TV", "improvement"), deps, h.hooks());
    CHECK(has_paper_review("TV"));  // 声が無い → レビュー
    CHECK(handle_paper_review_approval("TV", deps, h.hooks(), std::string("G1")));
    CHECK(w5u::drain(h.ex));
    const auto posts = h.discord.post_contents("TV");
    CHECK_EQ(posts.back(), std::string("✅ **結論**\n(明確な結論には至りませんでした)"));
    CHECK_EQ(h.dispatched[0].flow, std::string("improvement"));
    CHECK_EQ(h.concluded.size(), std::size_t{1});
}

TEST_CASE("準備中の返信は溜めて草案の後に届いた順に処理する・同じスレッドで準備を二重に始めない") {
    w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_ENABLED", "true"}});
    reset_flow_live_for_test();
    Harness h;
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion();
    std::atomic<bool> second_rejected{false};
    deps.prepareUserVoices = [&](const user_voices::PrepareUserVoicesArgs&) {
        // 準備中: 返信は溜まり (true)、承認は草案前なので false、同じスレッドの再起動は何も投稿しない。
        CHECK(is_preparing("TB"));
        CHECK(handle_paper_review_reply("TB", "G1", "開始", deps));
        CHECK(!handle_paper_review_approval("TB", deps));
        const auto before = h.discord.post_count();
        start_forum_flow(input("TB", "discussion"), deps);
        second_rejected = h.discord.post_count() == before;
        return user_voices::PreparedUserVoices{};
    };
    start_forum_flow(input("TB", "discussion"), deps);
    CHECK(second_rejected.load());
    CHECK(w5u::drain(h.ex));
    const auto posts = h.discord.post_contents("TB");
    // 草案 → 声の準備 → 案内 の後に、溜めた「開始」で承認される。
    CHECK(posts.size() >= 5);
    CHECK(str::starts_with(posts[1], "📝 **ディスカッションペーパー"));
    CHECK(str::starts_with(posts[3], "✏️"));
    CHECK_EQ(posts[4], std::string("✅ ペーパーを承認しました。議論を始めます…"));
    CHECK_EQ(h.dispatched.size(), std::size_t{1});
    CHECK_EQ(detail::serial_queue_count(), std::size_t{0});
}

DI_TEST_MAIN()
