// w5u_gateway のテスト (1/4)。準備は w5u_gateway_support.hpp。
#include "w5u_gateway_support.hpp"

using namespace w5u_gateway_support;

TEST_CASE("純関数: テキスト系添付・モーダルの値・進行量の数値") {
    CHECK(is_text_like_attachment("spec.MD", std::nullopt));
    CHECK(is_text_like_attachment("a.yml", std::nullopt));
    CHECK(is_text_like_attachment("x.bin", std::string("text/plain")));
    CHECK(is_text_like_attachment("x.bin", std::string("application/json; charset=utf-8")));
    CHECK(!is_text_like_attachment("x.png", std::string("image/png")));
    CHECK(!is_text_like_attachment("x.mdx", std::nullopt));
    CHECK(!is_text_like_attachment("x.bin", std::string("Text/plain")));  // startsWith は大文字小文字を区別する
    const Json data{{"custom_id", "flow-modal:T1"},
                    {"components", Json::array({Json{{"type", 1}, {"components", Json::array({Json{{"type", 4}, {"custom_id", "rounds"}, {"value", " 3 "}}})}},
                                                Json{{"type", 18}, {"component", Json{{"type", 4}, {"custom_id", "turns"}, {"value", "6"}}}}})}};
    const auto f = modal_field_values(data);
    CHECK_EQ(f.at("rounds"), std::string(" 3 "));
    CHECK_EQ(f.at("turns"), std::string("6"));
    CHECK(modal_field_values(Json::array()).empty());
    CHECK_EQ(positive_number_field(" 3 ").value_or(0), 3.0);
    CHECK_EQ(positive_number_field("2.5").value_or(0), 2.5);
    CHECK(!positive_number_field(""));
    CHECK(!positive_number_field("0"));
    CHECK(!positive_number_field("-1"));
    CHECK(!positive_number_field("abc"));
    CHECK(!positive_number_field("Infinity"));
    // 深い入れ子 (上限 8 より深い) は辿らない
    Json deep{{"custom_id", "x"}, {"value", "v"}};
    for (int i = 0; i < 20; ++i) deep = Json{{"components", Json::array({deep})}};
    CHECK(modal_field_values(deep).empty());
}

TEST_CASE("bot token が空なら起動しない") {
    DiscordGatewayDeps d;
    CHECK(start_discord_gateway(std::move(d)) == nullptr);
}

TEST_CASE("READY: slash 登録・運用チャンネル・議論フォーラムの解決 (READY は 1 度だけ)") {
    GwHarness h;
    auto gw = h.start(h.deps());
    const auto ids = gw->managed_forum_ids();
    CHECK_EQ(ids.size(), std::size_t{1});
    CHECK_EQ(ids.at("G1"), std::string("F1"));
    CHECK_EQ(h.discord.commands_registered, 1);
    const auto crawl = gw->crawl_channel_ids();
    CHECK(std::find(crawl.begin(), crawl.end(), "dl") != crawl.end());
    // 再 identify の READY では繰り返さない (discord.js の client.once)。
    gw->handle_dispatch("READY", Json{{"user", Json{{"id", "bot"}}}});
    h.settle(*gw);
    CHECK_EQ(h.discord.commands_registered, 1);
}

TEST_CASE("READY: 議論フォーラム名が空なら guild を拒否し、全イベントが門で落ちる (fail closed)") {
    GwHarness h;
    auto d = h.deps();
    d.forum->discussionForumName = "  ";
    auto gw = h.start(std::move(d));
    CHECK(gw->managed_forum_ids().empty());
    h.discord.add_thread("T1");
    h.discord.add_starter("T1", "本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("T1", "F1", "周回 ラウンド:2", {h.tag_id("議論")}));
    gw->handle_dispatch("INTERACTION_CREATE", slash("i1", "T1", "discutere-status", "u1"));
    h.settle(*gw);
    CHECK_EQ(h.dispatch_count(), std::size_t{0});
    CHECK(h.discord.snapshot(h.discord.callbacks).empty());
    CHECK(gw->event_stats().gateRejected >= 2);
}

TEST_CASE("READY: guild 別の議論フォーラム名を共通の名前より優先する") {
    GwHarness h;
    auto d = h.deps();
    d.forum->discussionForumName = "存在しない共通名";
    d.forum->discussionForumNameByGuild["G1"] = "議論";
    auto gw = h.start(std::move(d));
    CHECK_EQ(gw->managed_forum_ids().size(), std::size_t{1});
    CHECK(gw->managed_forum_ids().count("G1") == 1);
}

TEST_CASE("READY: guild 別の指定が空白なら共通の名前を使う") {
    GwHarness h;
    auto d = h.deps();
    d.forum->discussionForumNameByGuild["G1"] = "  ";
    auto gw = h.start(std::move(d));
    CHECK(gw->managed_forum_ids().count("G1") == 1);
}

DI_TEST_MAIN()
