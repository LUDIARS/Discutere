// w5u_gateway のテスト (4/4)。準備は w5u_gateway_support.hpp。
#include "w5u_gateway_support.hpp"

using namespace w5u_gateway_support;

TEST_CASE("finalize / react: 管理外の scene・チャンネルには何もしない") {
    GwHarness h;
    auto gw = h.start(h.deps());
    h.discord.add_thread("TX", "F9");
    const auto r = gw->finalize_forum_post(std::string("discord:G1/TX"), "まとめ", "題");
    CHECK(!r.closed);
    CHECK_EQ(r.reason.value_or(""), std::string("outside managed forum"));
    CHECK(!gw->finalize_forum_post(std::string("web:room"), "まとめ", "題").closed);
    gw->react_to_message("TX", "m1", "👍");
    h.discord.add_thread("T1");
    gw->react_to_message("T1", "m1", "👍");
    const auto reactions = h.discord.snapshot(h.discord.reactions);
    CHECK_EQ(reactions.size(), std::size_t{1});
    CHECK(str::starts_with(reactions[0], "T1/m1/"));
}

TEST_CASE("キュー: 通常キューが満杯でも起動系 (THREAD_CREATE) は落ちない・満杯は数える") {
    GwHarness h;
    auto d = h.deps();
    d.normalThreads = 1;
    d.normalQueue = 1;
    auto gw = h.start(std::move(d));
    h.discord.add_thread("TQ");
    // 通常キューを詰まらせる (大量の MESSAGE_CREATE)
    for (int i = 0; i < 200; ++i) gw->handle_dispatch("MESSAGE_CREATE", message("q" + std::to_string(i), "TQ", "x"));
    h.discord.add_thread("T1", "F1", "起動 ラウンド:2");
    h.discord.add_starter("T1", "本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("T1", "F1", "起動 ラウンド:2", {h.tag_id("議論")}));
    h.settle(*gw);
    const auto s = gw->event_stats();
    CHECK(s.normalDropped > 0);
    CHECK_EQ(s.priorityDropped, std::uint64_t{0});
    CHECK_EQ(h.dispatch_count(), std::size_t{1});
}

TEST_CASE("E2E: 偽 Gateway → READY → THREAD_CREATE → 起動 → 結論 → lock/archive。token をログに出さない") {
    LogCapture logs;
    GwHarness h;
    w5u::FakeGateway fg;
    {
        std::lock_guard lock(h.discord.m);
        h.discord.gateway_url = fg.url();
    }
    auto d = h.deps();
    d.connect = true;
    d.gatewayOptions.rest = h.discord.options();
    d.gatewayOptions.allow_insecure_test_endpoints = true;
    d.gatewayOptions.reconnect_base = std::chrono::milliseconds(10);
    d.gatewayOptions.identify_window = std::chrono::milliseconds(0);
    d.gatewayOptions.close_grace = std::chrono::milliseconds(300);
    auto gw = start_discord_gateway(std::move(d));
    CHECK(gw != nullptr);
    auto conn = fg.next();
    CHECK(conn != nullptr);
    CHECK(conn->send(Json{{"op", 10}, {"d", Json{{"heartbeat_interval", 10'000}}}}));
    auto identify = conn->recv_op(2);
    CHECK(identify.has_value());
    CHECK_EQ((*identify)["d"]["token"].get<std::string>(), std::string(kToken));
    CHECK(conn->send(Json{{"op", 0}, {"t", "READY"}, {"s", 1},
                          {"d", Json{{"session_id", "s1"}, {"resume_gateway_url", fg.url()}, {"user", Json{{"id", "bot"}, {"username", "di-bot"}}},
                                     {"application", Json{{"id", "app"}}}}}}));
    CHECK(w5u::wait_until([&] { return gw->ready_done(); }));
    h.discord.add_thread("T1", "F1", "E2E ラウンド:2");
    h.discord.add_starter("T1", "本文");
    CHECK(conn->send(Json{{"op", 0}, {"t", "THREAD_CREATE"}, {"s", 2}, {"d", thread_create("T1", "F1", "E2E ラウンド:2", {h.tag_id("議論")})}}));
    // 同じイベントの再送 (resume) は 1 度だけ処理する。
    CHECK(conn->send(Json{{"op", 0}, {"t", "THREAD_CREATE"}, {"s", 3}, {"d", thread_create("T1", "F1", "E2E ラウンド:2", {h.tag_id("議論")})}}));
    CHECK(w5u::wait_until([&] {
        for (const auto& p : h.discord.snapshot(h.discord.patches))
            if (p["channel"] == "T1" && p["body"].value("archived", false)) return true;
        return false;
    }));
    h.settle(*gw);
    CHECK_EQ(h.dispatch_count(), std::size_t{1});
    CHECK(gw->gateway_stats().duplicates_dropped >= 1);
    gw->stop();
    CHECK(!logs.contains(kToken));
    CHECK(!logs.contains("fake-webhook-token"));
}

DI_TEST_MAIN()
