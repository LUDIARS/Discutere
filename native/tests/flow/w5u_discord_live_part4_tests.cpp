// w5u_discord_live のテスト (4/4)。準備は w5u_discord_live_support.hpp。
#include "w5u_discord_live_support.hpp"

using namespace w5u_discord_live_support;

TEST_CASE("壁打ち: 起動 → 返信で応答 → /終了 で締めて onConcluded") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    Harness h;
    h.discord.add_thread("TS");
    auto deps = h.deps(env.games());
    h.llm = std::make_shared<w5u::FakeLLM>(std::string("反証: その主張には前提がある。"));
    deps.llm = h.llm;
    deps.dispatch = sparring_dispatch();
    start_forum_flow(input("TS", "sparring"), deps, h.hooks());
    CHECK(has_sparring_session("TS"));
    CHECK(str::starts_with(h.discord.post_contents("TS").back(), "🥊 **壁打ち**を始めます"));
    CHECK(handle_forum_flow_reply("TS", "G1", "周回は長いほうが面白い", deps, h.hooks()));
    CHECK(!h.discord.snapshot(h.discord.webhooks).empty());  // AI の応答は webhook で persona 名
    CHECK(handle_forum_flow_reply("TS", "G1", "/終了", deps, h.hooks()));
    CHECK(!has_sparring_session("TS"));
    CHECK(str::starts_with(h.discord.post_contents("TS").back(), "✅ **壁打ち終了**\n"));
    CHECK_EQ(h.concluded.size(), std::size_t{1});
    CHECK(str::starts_with(h.concluded[0], "discord:G1/TS|壁打ち|"));
    CHECK(!handle_forum_flow_reply("TS", "G1", "まだ?", deps));  // 終わった壁打ちは取り込まない
}

TEST_CASE("境界: 空返信・巨大な返信・1900 文字の切り詰め・背景キュー満杯") {
    w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_ENABLED", "true"}});
    reset_flow_live_for_test();
    Harness h;
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion();
    start_forum_flow(input("TX", "discussion"), deps);
    const auto before = h.discord.post_count();
    CHECK(handle_paper_review_reply("TX", "G1", "   ", deps));  // 空返信は無視 (レビューは継続)
    CHECK_EQ(h.discord.post_count(), before);
    // 巨大な調整指示 (1MB) — LLM は失敗し、元のペーパーを保って「編集に失敗」を返す。
    CHECK(handle_paper_review_reply("TX", "G1", std::string(1u << 20, 'a'), deps));
    CHECK(has_paper_review("TX"));
    // 背景キューが満杯なら承認しても議論を始めず、レビュー待ちに戻す。
    deps.submitBackground = [](std::function<void()>) { return false; };
    CHECK(handle_paper_review_reply("TX", "G1", "開始", deps));
    CHECK(str::starts_with(h.discord.post_contents("TX").back(), "⚠️ 混雑のため議論を開始できませんでした"));
    CHECK(has_paper_review("TX"));
    CHECK(!is_discussion_running("TX"));
    CHECK_EQ(h.dispatched.size(), std::size_t{0});
    // 通知の本文は 1900 文字 (UTF-16) で切る (poster)。テーマを長くすると準備の通知が切られる。
    auto in = input("TY", "discussion");
    in.theme = std::string(5000, 'x');
    start_forum_flow(in, deps);
    for (const auto& c : h.discord.post_contents("TY")) CHECK(str::js_length(c) <= 1900);
}

TEST_CASE("API 未設定・LLM 未設定は投稿せずに warn で終わる (例外を外へ出さない)") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    FlowDiscordDeps deps;
    start_forum_flow(input("TZ", "discussion"), deps);
    CHECK(!has_paper_review("TZ"));
    CHECK(!is_preparing("TZ"));
}

DI_TEST_MAIN()
