// gateway_client のテスト (2/4)。準備は gateway_client_support.hpp。
#include "gateway_client_support.hpp"

using namespace gateway_client_support;

TEST_CASE("missing heartbeat ACK is a zombie connection: reconnect and resume (token never logged)") {
    LogCapture logs;
    FakeGateway gw;
    FakeRest rest(gw.url());
    Recorder rec;
    GatewayClient client(options_for(rest, rec));
    client.start();
    auto c0 = gw.next();
    CHECK(c0 != nullptr);
    handshake(*c0, gw.url(), 60, 2);
    // ACK を返さない → 次の heartbeat の時点でクライアントが切る
    CHECK(c0->wait_closed(2000ms));
    auto c1 = gw.next();
    CHECK(c1 != nullptr);
    CHECK(c1->send(hello(10'000)));
    auto resume = c1->recv_op(gateway_op::Resume);
    CHECK(resume.has_value());
    CHECK_EQ((*resume)["d"]["session_id"].get<std::string>(), std::string("sess-1"));
    CHECK_EQ((*resume)["d"]["seq"].get<std::int64_t>(), std::int64_t(2));
    CHECK_EQ((*resume)["d"]["token"].get<std::string>(), std::string(kToken));
    CHECK(c1->send(dispatch("RESUMED", 3, Json::object())));
    CHECK(rec.wait_count("RESUMED", 1));
    CHECK(client.stats().zombie_reconnects >= 1);
    CHECK_EQ(rest.calls.load(), 1);  // resume は /gateway/bot を取り直さない
    client.stop();
    CHECK(logs.text().find(kToken) == std::string::npos);
    CHECK(logs.text().find("zombie") != std::string::npos);
}

TEST_CASE("op 7 reconnect and op 9 invalid session (true → resume, false → identify)") {
    FakeGateway gw;
    FakeRest rest(gw.url());
    Recorder rec;
    GatewayClient client(options_for(rest, rec));
    client.start();
    auto c0 = gw.next();
    CHECK(c0 != nullptr);
    handshake(*c0, gw.url());
    CHECK(c0->send(Json{{"op", 7}, {"d", nullptr}}));
    auto c1 = gw.next();
    CHECK(c1 != nullptr);
    CHECK(c1->send(hello(10'000)));
    CHECK(c1->recv_op(gateway_op::Resume).has_value());
    // op 9 d=true → resume
    CHECK(c1->send(Json{{"op", 9}, {"d", true}}));
    auto c2 = gw.next();
    CHECK(c2 != nullptr);
    CHECK(c2->send(hello(10'000)));
    CHECK(c2->recv_op(gateway_op::Resume).has_value());
    // op 9 d=false → session を捨てて identify (/gateway/bot を取り直す)
    CHECK(c2->send(Json{{"op", 9}, {"d", false}}));
    auto c3 = gw.next();
    CHECK(c3 != nullptr);
    CHECK(c3->send(hello(10'000)));
    CHECK(c3->recv_op(gateway_op::Identify).has_value());
    CHECK_EQ(rest.calls.load(), 2);
    CHECK(!client.stats().has_session);
    client.stop();
}

TEST_CASE("close 4004 stops without reconnecting; 401 on /gateway/bot is fatal too") {
    {
        FakeGateway gw;
        FakeRest rest(gw.url());
        Recorder rec;
        std::atomic<int> fatal_code{0};
        auto o = options_for(rest, rec);
        o.on_fatal = [&](int code, const std::string&) { fatal_code = code; };
        GatewayClient client(o);
        client.start();
        auto c0 = gw.next();
        CHECK(c0 != nullptr);
        CHECK(c0->send(hello(10'000)));
        CHECK(c0->recv_op(gateway_op::Identify).has_value());
        c0->close(4004);
        for (int i = 0; i < 100 && client.running(); ++i) std::this_thread::sleep_for(20ms);
        CHECK(!client.running());
        CHECK_EQ(fatal_code.load(), 4004);
        CHECK(gw.next(300ms) == nullptr);
        CHECK_EQ(gw.accepted.load(), 1);
    }
    {
        FakeGateway gw;
        FakeRest rest(gw.url());
        rest.status = 401;
        Recorder rec;
        std::atomic<int> fatal_code{0};
        auto o = options_for(rest, rec);
        o.on_fatal = [&](int code, const std::string&) { fatal_code = code; };
        GatewayClient client(o);
        client.start();
        for (int i = 0; i < 100 && client.running(); ++i) std::this_thread::sleep_for(20ms);
        CHECK(!client.running());
        CHECK_EQ(fatal_code.load(), 401);
        CHECK_EQ(gw.accepted.load(), 0);
    }
}

TEST_CASE("close 4000 resumes; close 4009 identifies again") {
    FakeGateway gw;
    FakeRest rest(gw.url());
    Recorder rec;
    GatewayClient client(options_for(rest, rec));
    client.start();
    auto c0 = gw.next();
    CHECK(c0 != nullptr);
    handshake(*c0, gw.url());
    c0->close(4000);
    auto c1 = gw.next();
    CHECK(c1 != nullptr);
    CHECK(c1->send(hello(10'000)));
    CHECK(c1->recv_op(gateway_op::Resume).has_value());
    c1->close(4009);
    auto c2 = gw.next();
    CHECK(c2 != nullptr);
    CHECK(c2->send(hello(10'000)));
    CHECK(c2->recv_op(gateway_op::Identify).has_value());
    CHECK(client.running());
    client.stop();
}

DI_TEST_MAIN()
