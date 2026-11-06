// w5u_gateway のテスト (3/4)。準備は w5u_gateway_support.hpp。
#include "w5u_gateway_support.hpp"

using namespace w5u_gateway_support;

TEST_CASE("admin コマンド: allowlist 未設定は全拒否・不一致は admin only・管理外チャンネルの slash は応答しない") {
    GwHarness h;
    auto gw = h.start(h.deps());
    h.discord.add_thread("T1");
    gw->handle_dispatch("INTERACTION_CREATE", slash("a1", "T1", "discutere-kill", "u1"));
    h.settle(*gw);
    auto cb = callback_for(h.discord, "a1");
    CHECK_EQ(cb["data"]["content"].get<std::string>(), std::string("⚠️ discord.adminIds not set — admin commands are disabled"));
    CHECK_EQ(cb["data"]["flags"].get<int>(), 64);
    gw->stop();

    GwHarness h2;
    auto d = h2.deps();
    d.router.adminIds = {"admin1"};
    auto gw2 = h2.start(std::move(d));
    h2.discord.add_thread("T1");
    h2.discord.add_thread("TX", "F9");
    gw2->handle_dispatch("INTERACTION_CREATE", slash("a2", "T1", "discutere-kill", "u1"));
    gw2->handle_dispatch("INTERACTION_CREATE", slash("a3", "TX", "discutere-kill", "admin1"));
    h2.settle(*gw2);
    CHECK_EQ(callback_for(h2.discord, "a2")["data"]["content"].get<std::string>(), std::string("⚠️ admin only"));
    CHECK(callback_for(h2.discord, "a3").is_null());  // 管理外フォーラムのスレッド
}

TEST_CASE("ペーパーレビュー: 返信に ☑️ → 調整、✅ 承認 (bot 自身・他 bot の ✅ は無視)、reply 投稿は無視") {
    GwHarness h({{"DISCUTERE_FLOW_PAPER_REVIEW_ENABLED", "true"}});
    auto gw = h.start(h.deps());
    h.discord.add_thread("TR", "F1", "レビュー ラウンド:2");
    h.discord.add_starter("TR", "本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("TR", "F1", "レビュー ラウンド:2", {h.tag_id("議論")}));
    h.settle(*gw);
    CHECK(flow::has_paper_review("TR"));
    const auto before = h.discord.post_count();

    // reply 投稿 (message_reference あり) は宛先によらず無視。
    Json reply = message("r1", "TR", "開始");
    reply["message_reference"] = Json{{"message_id", "x"}};
    gw->handle_dispatch("MESSAGE_CREATE", reply);
    // bot / webhook の投稿も無視。
    gw->handle_dispatch("MESSAGE_CREATE", message("r2", "TR", "開始", "bot", true));
    Json hook = message("r3", "TR", "開始", "wh");
    hook["webhook_id"] = "wh";
    gw->handle_dispatch("MESSAGE_CREATE", hook);
    // bot 自身・他 bot の ✅ は承認にしない。
    gw->handle_dispatch("MESSAGE_REACTION_ADD", Json{{"user_id", "bot"}, {"channel_id", "TR"}, {"message_id", "m1"}, {"guild_id", "G1"},
                                                     {"emoji", Json{{"name", "✅"}}}});
    gw->handle_dispatch("MESSAGE_REACTION_ADD", Json{{"user_id", "b2"}, {"channel_id", "TR"}, {"message_id", "m1"}, {"guild_id", "G1"},
                                                     {"member", Json{{"user", Json{{"id", "b2"}, {"bot", true}}}}}, {"emoji", Json{{"name", "✅"}}}});
    h.settle(*gw);
    CHECK_EQ(h.discord.post_count(), before);
    CHECK_EQ(h.dispatch_count(), std::size_t{0});

    // 通常の返信 (相づち) → ☑️ を付けるがペーパーは書き換えない。
    gw->handle_dispatch("MESSAGE_CREATE", message("r4", "TR", "はい"));
    h.settle(*gw);
    const auto reactions = h.discord.snapshot(h.discord.reactions);
    CHECK(!reactions.empty() && str::starts_with(reactions.back(), "TR/r4/"));
    CHECK_EQ(h.discord.post_count(), before);

    // 人の ✅ で承認 → 議論。
    gw->handle_dispatch("MESSAGE_REACTION_ADD", Json{{"user_id", "u1"}, {"channel_id", "TR"}, {"message_id", "m1"}, {"guild_id", "G1"},
                                                     {"member", Json{{"user", Json{{"id", "u1"}, {"bot", false}}}}}, {"emoji", Json{{"name", "✅"}}}});
    h.settle(*gw);
    CHECK_EQ(h.dispatch_count(), std::size_t{1});
    CHECK(!flow::has_paper_review("TR"));
    // 承認後の 👍 はスコアリングへ (onReaction)
    gw->handle_dispatch("MESSAGE_REACTION_ADD", Json{{"user_id", "u1"}, {"channel_id", "TR"}, {"message_id", "m9"}, {"guild_id", "G1"},
                                                     {"emoji", Json{{"name", "👍"}}}});
    h.settle(*gw);
    CHECK_EQ(h.reactions_seen.size(), std::size_t{1});
    CHECK_EQ(h.reactions_seen[0].emoji, std::string("👍"));
}

TEST_CASE("壁打ち: スレッドへの返信を相手発話として取り込み、webhook で応答する") {
    GwHarness h;
    h.llm = std::make_shared<w5u::FakeLLM>(std::string("その主張の前提は何か。"));
    auto gw = h.start(h.deps());
    h.discord.add_thread("TP", "F1", "壁打ちテーマ");
    h.discord.add_starter("TP", "本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("TP", "F1", "壁打ちテーマ", {"tag-spar"}));
    h.settle(*gw);
    CHECK(flow::has_sparring_session("TP"));
    gw->handle_dispatch("MESSAGE_CREATE", message("p1", "TP", "周回は長いほうが良い"));
    h.settle(*gw);
    CHECK(!h.discord.snapshot(h.discord.webhooks).empty());
}

TEST_CASE("bot メンションは調整指示として返信する (空の指示は KG を開かずに案内)・管理外では無視") {
    GwHarness h;
    auto gw = h.start(h.deps());
    h.discord.add_thread("TM");
    Json msg = message("d1", "TM", "<@bot>");
    msg["mentions"] = Json::array({Json{{"id", "bot"}, {"username", "di-bot"}}});
    gw->handle_dispatch("MESSAGE_CREATE", msg);
    h.discord.add_thread("TX", "F9");
    Json outside = message("d2", "TX", "<@bot>");
    outside["mentions"] = msg["mentions"];
    gw->handle_dispatch("MESSAGE_CREATE", outside);
    h.settle(*gw);
    const auto posts = h.discord.snapshot(h.discord.posts);
    CHECK_EQ(posts.size(), std::size_t{1});
    CHECK_EQ(posts[0]["channel"].get<std::string>(), std::string("TM"));
    CHECK_EQ(posts[0]["message_reference"]["message_id"].get<std::string>(), std::string("d1"));
}

DI_TEST_MAIN()
