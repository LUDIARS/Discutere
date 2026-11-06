// gateway_client のテスト (4/4)。準備は gateway_client_support.hpp。
#include "gateway_client_support.hpp"

using namespace gateway_client_support;

TEST_CASE("no Hello within the deadline reconnects") {
    FakeGateway gw;
    FakeRest rest(gw.url());
    Recorder rec;
    auto o = options_for(rest, rec);
    o.hello_timeout = 150ms;
    GatewayClient client(o);
    client.start();
    auto c0 = gw.next();
    CHECK(c0 != nullptr);
    CHECK(c0->wait_closed(2000ms));
    auto c1 = gw.next();
    CHECK(c1 != nullptr);
    CHECK(c1->send(hello(10'000)));
    CHECK(c1->recv_op(gateway_op::Identify).has_value());
    client.stop();
}

TEST_CASE("session_start_limit and max_concurrency delay identify") {
    FakeGateway gw;
    FakeRest rest(gw.url());
    rest.remaining = 0;
    rest.reset_after = 300;
    Recorder rec;
    auto o = options_for(rest, rec);
    o.identify_window = 400ms;
    GatewayClient client(o);
    const auto t0 = Clock::now();
    client.start();
    auto c0 = gw.next();
    CHECK(c0 != nullptr);
    CHECK(Clock::now() - t0 >= 280ms);  // reset_after を待ってから接続した
    CHECK_EQ(rest.calls.load(), 2);
    CHECK(c0->send(hello(10'000)));
    CHECK(c0->recv_op(gateway_op::Identify).has_value());
    const auto t1 = Clock::now();
    CHECK(c0->send(Json{{"op", 9}, {"d", false}}));
    auto c1 = gw.next();
    CHECK(c1 != nullptr);
    CHECK(c1->send(hello(10'000)));
    CHECK(c1->recv_op(gateway_op::Identify).has_value());
    CHECK(Clock::now() - t1 >= 350ms);  // max_concurrency=1: identify_window に 1 回
    client.stop();
}

TEST_CASE("handlers run on the executor; a full queue drops the event with a warning") {
    LogCapture logs;
    FakeGateway gw;
    FakeRest rest(gw.url());
    Recorder rec;
    Executor ex("gw-test", 1, 1);
    auto o = options_for(rest, rec);
    o.dispatch_in_background = true;
    o.executor = &ex;
    {
        GatewayClient client(o);
        client.start();
        auto c0 = gw.next();
        CHECK(c0 != nullptr);
        handshake(*c0, gw.url());
        CHECK(rec.wait_count("READY", 1));
        // executor を塞ぐ (実行中 1 + キュー 1)
        std::mutex block;
        std::unique_lock hold(block);
        std::atomic<bool> busy{false};
        CHECK(ex.submit([&] {
            busy = true;
            std::lock_guard l(block);
        }));
        for (int i = 0; i < 100 && !busy; ++i) std::this_thread::sleep_for(5ms);
        CHECK(ex.submit([] {}));
        CHECK(c0->send(dispatch("MESSAGE_CREATE", 2, Json{{"id", "dropped"}})));
        for (int i = 0; i < 100 && client.stats().dropped_queue_full == 0; ++i) std::this_thread::sleep_for(10ms);
        CHECK_EQ(client.stats().dropped_queue_full, std::uint64_t(1));
        hold.unlock();
        for (int i = 0; i < 200 && (ex.queued() > 0 || ex.running() > 0); ++i) std::this_thread::sleep_for(5ms);
        CHECK(c0->send(dispatch("MESSAGE_CREATE", 3, Json{{"id", "kept"}})));
        CHECK(rec.wait_count("MESSAGE_CREATE", 1));
        client.stop();
    }
    ex.stop();
    CHECK_EQ(rec.count("MESSAGE_CREATE"), 1);
    CHECK(logs.text().find("executor queue full") != std::string::npos);
}

TEST_CASE("stop returns promptly during backoff") {
    FakeGateway gw;
    FakeRest rest(gw.url());
    rest.status = 500;
    Recorder rec;
    auto o = options_for(rest, rec);
    o.reconnect_base = 5'000ms;
    o.reconnect_max = 10'000ms;
    GatewayClient client(o);
    client.start();
    for (int i = 0; i < 100 && rest.calls.load() == 0; ++i) std::this_thread::sleep_for(10ms);
    CHECK_EQ(rest.calls.load(), 1);
    std::this_thread::sleep_for(50ms);
    CHECK(client.running());
    const auto t0 = Clock::now();
    client.stop();
    CHECK(Clock::now() - t0 < 1000ms);
    CHECK(!client.running());
    client.stop();  // 2 度目も安全
}

DI_TEST_MAIN()
