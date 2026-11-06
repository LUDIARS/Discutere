// gateway_client のテスト (1/4)。準備は gateway_client_support.hpp。
#include "gateway_client_support.hpp"

using namespace gateway_client_support;

// ─── 純関数・部品 ───────────────────────────────────────────

TEST_CASE("close codes, urls and intents") {
    CHECK(classify_close_code(4004) == GatewayCloseAction::Fatal);
    for (int c = 4010; c <= 4014; ++c) CHECK(classify_close_code(c) == GatewayCloseAction::Fatal);
    CHECK(classify_close_code(4000) == GatewayCloseAction::Resume);
    CHECK(classify_close_code(4008) == GatewayCloseAction::Resume);
    CHECK(classify_close_code(1006) == GatewayCloseAction::Resume);
    CHECK(classify_close_code(1009) == GatewayCloseAction::Resume);
    CHECK(classify_close_code(4007) == GatewayCloseAction::Reidentify);
    CHECK(classify_close_code(4009) == GatewayCloseAction::Reidentify);
    CHECK(classify_close_code(1000) == GatewayCloseAction::Reidentify);

    CHECK_EQ(build_gateway_url("wss://gateway.discord.gg"), std::string("wss://gateway.discord.gg/?v=10&encoding=json"));
    CHECK_EQ(build_gateway_url("wss://gateway.discord.gg/?v=9&encoding=etf"), std::string("wss://gateway.discord.gg/?v=10&encoding=json"));
    CHECK(is_allowed_gateway_url("wss://gateway.discord.gg/?v=10", false));
    CHECK(is_allowed_gateway_url("wss://gateway-us-east1-b.discord.gg", false));
    CHECK(is_allowed_gateway_url("wss://GATEWAY.Discord.GG", false));
    CHECK(!is_allowed_gateway_url("ws://gateway.discord.gg", false));
    CHECK(!is_allowed_gateway_url("wss://evil-discord.gg", false));
    CHECK(!is_allowed_gateway_url("wss://discord.gg.evil.com", false));
    CHECK(!is_allowed_gateway_url("wss://127.0.0.1:1234", false));
    CHECK(!is_allowed_gateway_url("https://gateway.discord.gg", false));
    CHECK(!is_allowed_gateway_url("not a url", true));
    CHECK(is_allowed_gateway_url("ws://127.0.0.1:1234", true));

    // gateway.ts の Client 生成と同じ: Guilds | GuildMessages | MessageContent | DirectMessages | GuildMessageReactions
    CHECK_EQ(DEFAULT_GATEWAY_INTENTS, std::uint64_t(1 | 512 | 32768 | 4096 | 1024));
}

TEST_CASE("send limiter keeps reserved slots for heartbeats") {
    GatewaySendLimiter lim(120, 5, 60'000ms);
    const auto t0 = Clock::now();
    int normal = 0;
    while (lim.try_acquire(false, t0)) ++normal;
    CHECK_EQ(normal, 115);
    int prio = 0;
    while (lim.try_acquire(true, t0)) ++prio;
    CHECK_EQ(prio, 5);
    CHECK(!lim.try_acquire(true, t0));
    CHECK(lim.next_free(true, t0) == t0 + 60'000ms);
    CHECK(lim.next_free(false, t0) == t0 + 60'000ms);
    // 窓が過ぎれば空く (上限ちょうど・上限+1)
    CHECK(!lim.try_acquire(false, t0 + 59'999ms));
    CHECK(lim.try_acquire(false, t0 + 60'000ms));
    CHECK_EQ(lim.used(t0 + 60'000ms), std::size_t(1));
}

TEST_CASE("dedupe set: ttl, capacity and keys") {
    GatewayDedupe d(3, 1000ms);
    const auto t0 = Clock::now();
    CHECK(d.first_seen("a", t0));
    CHECK(!d.first_seen("a", t0 + 10ms));
    CHECK(d.first_seen("b", t0));
    CHECK(d.first_seen("c", t0));
    CHECK(d.first_seen("d", t0));  // 容量 3 を超えたので a を忘れる
    CHECK_EQ(d.size(), std::size_t(3));
    CHECK(d.first_seen("a", t0 + 20ms));
    CHECK(d.first_seen("b", t0 + 1000ms));  // ttl 切れ
    d.forget("b");
    CHECK(d.first_seen("b", t0 + 1001ms));

    CHECK_EQ(*gateway_dedupe_key("MESSAGE_CREATE", Json{{"id", "9"}}), std::string("MESSAGE_CREATE:9"));
    CHECK_EQ(*gateway_dedupe_key("THREAD_CREATE", Json{{"id", "7"}}), std::string("THREAD_CREATE:7"));
    CHECK(!gateway_dedupe_key("MESSAGE_UPDATE", Json{{"id", "9"}}).has_value());
    CHECK(!gateway_dedupe_key("MESSAGE_CREATE", Json{{"content", "x"}}).has_value());
    CHECK(!gateway_dedupe_key("MESSAGE_CREATE", Json("str")).has_value());
    const Json r = {{"message_id", "1"}, {"user_id", "2"}, {"emoji", Json{{"id", nullptr}, {"name", "✅"}}}};
    CHECK_EQ(*gateway_dedupe_key("MESSAGE_REACTION_ADD", r), std::string("MESSAGE_REACTION_ADD:1:2:✅"));
    CHECK_EQ(*gateway_dedupe_key("MESSAGE_REACTION_REMOVE", r), std::string("MESSAGE_REACTION_ADD:1:2:✅"));
}

// ─── 偽 Gateway との通し ──────────────────────────────────────

TEST_CASE("hello, identify, heartbeat/ack, server heartbeat request, dispatch and stop") {
    FakeGateway gw;
    FakeRest rest(gw.url());
    Recorder rec;
    auto o = options_for(rest, rec);
    GatewayClient client(o);
    CHECK(!client.send(gateway_op::PresenceUpdate, Json::object()));  // 接続前は送れない
    client.start();
    auto c = gw.next();
    CHECK(c != nullptr);
    CHECK(c->send(hello(80)));
    auto id = c->recv_op(gateway_op::Identify);
    CHECK(id.has_value());
    const Json& d = (*id)["d"];
    CHECK_EQ(d["token"].get<std::string>(), std::string(kToken));
    CHECK_EQ(d["intents"].get<std::uint64_t>(), DEFAULT_GATEWAY_INTENTS);
    CHECK_EQ(d["properties"]["browser"].get<std::string>(), std::string("discutere-native"));
    CHECK_EQ(rest.auth_seen, std::string("Bot ") + kToken);
    CHECK(c->send(ready(1, gw.url())));
    CHECK(rec.wait_count("READY", 1));
    CHECK(client.ready());
    // 定期の heartbeat は最新の seq を載せる。ACK を返し続ける間はゾンビ扱いされない。
    // (READY より先に送られた初回の heartbeat は d=null になりうるので、最後の 1 件で seq を確かめる)
    Json last_hb;
    for (int i = 0; i < 3; ++i) {
        auto hb = c->recv_op(gateway_op::Heartbeat, 1000ms);
        CHECK(hb.has_value());
        last_hb = *hb;
        CHECK(c->send(Json{{"op", 11}}));
    }
    CHECK_EQ(last_hb["d"].get<std::int64_t>(), std::int64_t(1));
    for (int i = 0; i < 100 && client.stats().heartbeat_acks < 3; ++i) std::this_thread::sleep_for(10ms);  // 最後の ACK の処理を待つ
    CHECK_EQ(client.stats().zombie_reconnects, std::uint64_t(0));
    CHECK(client.stats().heartbeat_acks >= 3);
    CHECK(client.stats().last_latency_ms >= 0);
    CHECK(c->send(dispatch("MESSAGE_CREATE", 2, Json{{"id", "m1"}, {"content", "hi"}})));
    CHECK(rec.wait_count("MESSAGE_CREATE", 1));
    CHECK_EQ(client.stats().seq, std::int64_t(2));
    // サーバからの heartbeat 要求 (op 1) には即座に応える
    const auto before = client.stats().heartbeats_sent;
    CHECK(c->send(Json{{"op", 1}, {"d", nullptr}}));
    std::optional<Json> hb;
    while (true) {  // 要求の前に溜まっていた定期の heartbeat (d=1) は読み飛ばす
        hb = c->recv_op(gateway_op::Heartbeat, 1000ms);
        if (!hb || (*hb)["d"] == Json(2)) break;
        CHECK(c->send(Json{{"op", 11}}));
    }
    CHECK(hb.has_value());
    // 送信の数え上げは送った後なので、偽サーバが先に受け取ることがある。少し待って確かめる。
    for (int i = 0; i < 100 && client.stats().heartbeats_sent <= before; ++i) std::this_thread::sleep_for(10ms);
    CHECK(client.stats().heartbeats_sent > before);
    const auto t0 = Clock::now();
    client.stop();
    CHECK(Clock::now() - t0 < 1500ms);
    CHECK(c->wait_closed());
    CHECK_EQ(c->close_code, 1000);
    CHECK(!client.running());
    CHECK(gw.next(200ms) == nullptr);
}

DI_TEST_MAIN()
