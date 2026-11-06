// gateway_client のテスト (3/4)。準備は gateway_client_support.hpp。
#include "gateway_client_support.hpp"

using namespace gateway_client_support;

TEST_CASE("duplicate events are delivered once (seq replay, ids, reactions, across resume)") {
    FakeGateway gw;
    FakeRest rest(gw.url());
    Recorder rec;
    GatewayClient client(options_for(rest, rec));
    client.start();
    auto c0 = gw.next();
    CHECK(c0 != nullptr);
    handshake(*c0, gw.url());
    const Json react = {{"message_id", "m1"}, {"user_id", "u1"}, {"emoji", Json{{"id", nullptr}, {"name", "✅"}}}};
    CHECK(c0->send(dispatch("MESSAGE_CREATE", 2, Json{{"id", "m1"}})));
    CHECK(c0->send(dispatch("MESSAGE_CREATE", 3, Json{{"id", "m1"}})));        // 同じ id
    CHECK(c0->send(dispatch("MESSAGE_CREATE", 2, Json{{"id", "m-other"}})));   // seq が戻った
    CHECK(c0->send(dispatch("THREAD_CREATE", 4, Json{{"id", "t1"}, {"newly_created", true}})));
    CHECK(c0->send(dispatch("THREAD_CREATE", 5, Json{{"id", "t1"}})));
    CHECK(c0->send(dispatch("INTERACTION_CREATE", 6, Json{{"id", "i1"}})));
    CHECK(c0->send(dispatch("INTERACTION_CREATE", 7, Json{{"id", "i1"}})));
    CHECK(c0->send(dispatch("MESSAGE_REACTION_ADD", 8, react)));
    CHECK(c0->send(dispatch("MESSAGE_REACTION_ADD", 9, react)));               // 重複
    CHECK(c0->send(dispatch("MESSAGE_REACTION_REMOVE", 10, react)));
    CHECK(c0->send(dispatch("MESSAGE_REACTION_ADD", 11, react)));              // 付け直しは渡す
    CHECK(c0->send(dispatch("MESSAGE_UPDATE", 12, Json{{"id", "m1"}})));
    CHECK(c0->send(dispatch("MESSAGE_UPDATE", 13, Json{{"id", "m1"}})));       // 対象外 (2 回渡す)
    CHECK(rec.wait_count("MESSAGE_UPDATE", 2));
    // 再接続 (resume) 後のサーバの再送
    CHECK(c0->send(Json{{"op", 7}, {"d", nullptr}}));
    auto c1 = gw.next();
    CHECK(c1 != nullptr);
    CHECK(c1->send(hello(10'000)));
    auto resume = c1->recv_op(gateway_op::Resume);
    CHECK(resume.has_value());
    CHECK_EQ((*resume)["d"]["seq"].get<std::int64_t>(), std::int64_t(13));
    CHECK(c1->send(dispatch("MESSAGE_CREATE", 13, Json{{"id", "m-replay"}})));  // seq <= 13 は捨てる
    CHECK(c1->send(dispatch("MESSAGE_CREATE", 14, Json{{"id", "m1"}})));        // 既読 id
    CHECK(c1->send(dispatch("MESSAGE_CREATE", 15, Json{{"id", "m2"}})));
    CHECK(c1->send(dispatch("RESUMED", 16, Json::object())));
    CHECK(rec.wait_count("RESUMED", 1));
    CHECK_EQ(rec.count("MESSAGE_CREATE"), 2);  // m1, m2
    CHECK_EQ(rec.count("THREAD_CREATE"), 1);
    CHECK_EQ(rec.count("INTERACTION_CREATE"), 1);
    CHECK_EQ(rec.count("MESSAGE_REACTION_ADD"), 2);
    CHECK_EQ(rec.count("MESSAGE_REACTION_REMOVE"), 1);
    const auto st = client.stats();
    CHECK_EQ(st.stale_seq_dropped, std::uint64_t(2));
    CHECK_EQ(st.duplicates_dropped, std::uint64_t(5));
    client.stop();
}

TEST_CASE("outgoing sends respect the rate limit and keep slots for heartbeats") {
    FakeGateway gw;
    FakeRest rest(gw.url());
    Recorder rec;
    auto o = options_for(rest, rec);
    o.send_limit = 6;
    o.send_reserved = 2;
    o.send_window = 600ms;
    GatewayClient client(o);
    client.start();
    auto c0 = gw.next();
    CHECK(c0 != nullptr);
    handshake(*c0, gw.url(), 60'000);
    CHECK(rec.wait_count("READY", 1));
    const Json presence = {{"since", nullptr}, {"activities", Json::array()}, {"status", "online"}, {"afk", false}};
    for (int i = 0; i < 10; ++i) CHECK(client.send(gateway_op::PresenceUpdate, presence));
    CHECK(!client.send(gateway_op::Identify, Json::object()));              // identify 等は外から送れない
    CHECK(!client.send(gateway_op::PresenceUpdate, Json{{"x", std::string(5000, 'a')}}));  // 1 送信 4096 バイト超
    // 最初の窓では identify (1) を除いた 6-2-1 = 3 件まで
    const auto t0 = Clock::now();
    int first_window = 0;
    while (Clock::now() - t0 < 400ms) {
        auto f = c0->recv(50ms);
        if (f && json_int(*f, "op") == gateway_op::PresenceUpdate) ++first_window;
    }
    CHECK(first_window <= 3);
    CHECK(first_window >= 1);
    int total = first_window;
    const auto t1 = Clock::now();
    while (total < 10 && Clock::now() - t1 < 4000ms) {
        auto f = c0->recv(100ms);
        if (f && json_int(*f, "op") == gateway_op::PresenceUpdate) ++total;
    }
    CHECK_EQ(total, 10);
    client.stop();
}

TEST_CASE("message size limit: exactly at limit is accepted, over limit reconnects with resume") {
    FakeGateway gw;
    FakeRest rest(gw.url());
    Recorder rec;
    auto o = options_for(rest, rec);
    o.max_message_bytes = 2048;
    GatewayClient client(o);
    client.start();
    auto c0 = gw.next();
    CHECK(c0 != nullptr);
    handshake(*c0, gw.url());
    auto padded = [](std::int64_t s, std::size_t size) {
        std::string head = R"({"op":0,"t":"MESSAGE_CREATE","s":)" + std::to_string(s) + R"(,"d":{"id":"m)" + std::to_string(s) + R"(","content":")";
        std::string tail = R"("}})";
        return head + std::string(size - head.size() - tail.size(), 'x') + tail;
    };
    const std::string exact = padded(2, 2048);
    CHECK_EQ(exact.size(), std::size_t(2048));
    CHECK(c0->send_text(exact));
    CHECK(rec.wait_count("MESSAGE_CREATE", 1));
    CHECK(c0->send_text(padded(3, 2049)));
    CHECK(c0->wait_closed());
    auto c1 = gw.next();
    CHECK(c1 != nullptr);
    CHECK(c1->send(hello(10'000)));
    auto resume = c1->recv_op(gateway_op::Resume);
    CHECK(resume.has_value());
    CHECK_EQ((*resume)["d"]["seq"].get<std::int64_t>(), std::int64_t(2));
    CHECK_EQ(rec.count("MESSAGE_CREATE"), 1);
    client.stop();
}

TEST_CASE("broken JSON frames are dropped without dropping the connection") {
    FakeGateway gw;
    FakeRest rest(gw.url());
    Recorder rec;
    GatewayClient client(options_for(rest, rec));
    client.start();
    auto c0 = gw.next();
    CHECK(c0 != nullptr);
    handshake(*c0, gw.url());
    CHECK(c0->send_text("{not json"));
    CHECK(c0->send_text("[1,2,3]"));
    CHECK(c0->send_text(R"({"op":0,"t":"MESSAGE_CREATE","s":2,"d":)" + std::string(100, '[') + std::string(100, ']') + "}"));  // 深さ超過
    CHECK(c0->send_text(""));
    CHECK(c0->send(dispatch("MESSAGE_CREATE", 2, Json{{"id", "ok"}})));
    CHECK(rec.wait_count("MESSAGE_CREATE", 1));
    CHECK_EQ(client.stats().malformed_frames, std::uint64_t(4));
    CHECK(gw.next(200ms) == nullptr);
    CHECK(client.ready());
    client.stop();
}

DI_TEST_MAIN()
