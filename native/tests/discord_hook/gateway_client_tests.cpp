// discord_hook/gateway_client (Discord Gateway v10) を、テスト内の偽 Gateway (Beast の WebSocket サーバ) と
// 偽 REST (GET /gateway/bot) で確かめる: Hello/heartbeat/ACK、ACK 欠落でのゾンビ検出と resume、op 7、op 9 (true/false)、
// 4004 で止まる、4000 で resume、4009 で identify、重複イベントの排除、送信レート制限、メッセージ上限、壊れた JSON、
// session_start_limit / max_concurrency、executor 満杯、stop。間隔はすべて短く注入する (全体で数秒)。
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "base/executor.hpp"
#include "base/log.hpp"
#include "check.hpp"
#include "discord_hook/gateway_client.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::discord_hook;
using namespace std::chrono_literals;

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;

namespace {
constexpr const char* kToken = "fake-gateway-token-not-real-0123";

// ─── 偽 Gateway ───────────────────────────────────────────────
// stream の操作はすべて io スレッド (非同期) で行い、テスト側は post して結果を待つ。

struct FakeConn : std::enable_shared_from_this<FakeConn> {
    explicit FakeConn(tcp::socket s) : ws(std::move(s)) {}
    websocket::stream<beast::tcp_stream> ws;
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> frames;
    bool closed = false;
    int close_code = 0;

    static asio::awaitable<void> reader(std::shared_ptr<FakeConn> self) {
        while (true) {
            beast::flat_buffer buffer;
            boost::system::error_code ec;
            co_await self->ws.async_read(buffer, asio::redirect_error(asio::use_awaitable, ec));
            std::lock_guard lock(self->m);
            if (ec) {
                self->closed = true;
                self->close_code = self->ws.reason().code;
                self->cv.notify_all();
                co_return;
            }
            self->frames.push_back(beast::buffers_to_string(buffer.data()));
            self->cv.notify_all();
        }
    }

    /** 次のフレーム (JSON)。期限切れ・切断は nullopt。 */
    std::optional<Json> recv(std::chrono::milliseconds timeout = 2000ms) {
        std::unique_lock lock(m);
        if (!cv.wait_for(lock, timeout, [&] { return !frames.empty() || closed; })) return std::nullopt;
        if (frames.empty()) return std::nullopt;
        auto text = std::move(frames.front());
        frames.pop_front();
        JsonLimits limits;
        return parse_json(text, limits);
    }

    /** op が一致するフレームまで読み飛ばす (heartbeat が挟まるため)。 */
    std::optional<Json> recv_op(int op, std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            auto f = recv(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
            if (!f) return std::nullopt;
            if (json_int(*f, "op", -1) == op) return f;
        }
        return std::nullopt;
    }

    bool send_text(std::string text) {
        auto p = std::make_shared<std::promise<bool>>();
        auto fut = p->get_future();
        auto data = std::make_shared<std::string>(std::move(text));
        asio::post(ws.get_executor(), [self = shared_from_this(), data, p] {
            self->ws.text(true);
            self->ws.async_write(asio::buffer(*data), [data, p](beast::error_code ec, std::size_t) { p->set_value(!ec); });
        });
        return fut.wait_for(2s) == std::future_status::ready && fut.get();
    }
    bool send(const Json& j) { return send_text(json_stringify(j)); }

    void close(int code) {
        auto p = std::make_shared<std::promise<void>>();
        auto fut = p->get_future();
        asio::post(ws.get_executor(), [self = shared_from_this(), code, p] {
            self->ws.async_close(websocket::close_reason(static_cast<websocket::close_code>(code)), [p](beast::error_code) { p->set_value(); });
        });
        fut.wait_for(2s);
    }

    bool wait_closed(std::chrono::milliseconds timeout = 2000ms) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return closed; });
    }
};

struct FakeGateway {
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work{io.get_executor()};
    tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
    std::thread thread;
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::shared_ptr<FakeConn>> pending;
    std::vector<std::weak_ptr<FakeConn>> all;
    std::atomic<int> accepted{0};

    FakeGateway() {
        asio::co_spawn(io, accept_loop(), asio::detached);
        thread = std::thread([this] { io.run(); });
    }
    ~FakeGateway() {
        asio::post(io, [this] {
            boost::system::error_code ec;
            acceptor.close(ec);
            std::lock_guard lock(m);
            for (auto& w : all)
                if (auto c = w.lock()) beast::get_lowest_layer(c->ws).socket().close(ec);
        });
        work.reset();
        thread.join();
    }
    std::string url() const { return "ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()); }

    asio::awaitable<void> accept_loop() {
        while (true) {
            boost::system::error_code ec;
            tcp::socket s = co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, ec));
            if (ec) co_return;
            auto conn = std::make_shared<FakeConn>(std::move(s));
            conn->ws.read_message_max(1u << 20);
            co_await conn->ws.async_accept(asio::redirect_error(asio::use_awaitable, ec));
            if (ec) continue;
            asio::co_spawn(io, FakeConn::reader(conn), asio::detached);
            ++accepted;
            std::lock_guard lock(m);
            all.push_back(conn);
            pending.push_back(conn);
            cv.notify_all();
        }
    }

    /** 次の接続 (来なければ nullptr)。 */
    std::shared_ptr<FakeConn> next(std::chrono::milliseconds timeout = 3000ms) {
        std::unique_lock lock(m);
        if (!cv.wait_for(lock, timeout, [&] { return !pending.empty(); })) return nullptr;
        auto c = pending.front();
        pending.pop_front();
        return c;
    }
};

// ─── 偽 REST ─────────────────────────────────────────────────

struct FakeRest {
    w1d::FakeServer server;
    std::mutex m;
    std::string gateway_url;
    int remaining = 1000;
    std::int64_t reset_after = 0;
    int max_concurrency = 1;
    int status = 200;
    std::atomic<int> calls{0};
    std::string auth_seen;

    explicit FakeRest(const std::string& url) : gateway_url(url) {
        server.router->get("/gateway/bot", [this](http::Request& r) {
            ++calls;
            std::lock_guard lock(m);
            auth_seen = r.headers.get("authorization").value_or("");
            if (status != 200) return http::Response::json(Json{{"message", "nope"}, {"code", 0}}, status);
            Json limit = {{"total", 1000}, {"remaining", remaining}, {"reset_after", reset_after}, {"max_concurrency", max_concurrency}};
            if (remaining == 0) remaining = 1000;  // 次の取り直しでは回復している
            return http::Response::json(Json{{"url", gateway_url}, {"shards", 1}, {"session_start_limit", limit}});
        });
        server.start();
    }
};

struct Recorder {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::pair<std::string, Json>> events;
    void add(const std::string& t, const Json& d) {
        std::lock_guard lock(m);
        events.emplace_back(t, d);
        cv.notify_all();
    }
    int count(const std::string& t) {
        std::lock_guard lock(m);
        int n = 0;
        for (auto& e : events) n += e.first == t;
        return n;
    }
    bool wait_count(const std::string& t, int n, std::chrono::milliseconds timeout = 2000ms) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] {
            int c = 0;
            for (auto& e : events) c += e.first == t;
            return c >= n;
        });
    }
};

GatewayClientOptions options_for(FakeRest& rest, Recorder& rec) {
    GatewayClientOptions o;
    o.token = kToken;
    o.rest.base_url = rest.server.base;
    o.rest.allow_private_network = true;
    o.rest.timeout = 2000ms;
    o.rest.max_retries = 0;
    o.allow_insecure_test_endpoints = true;
    o.dispatch_in_background = false;
    o.reconnect_base = 10ms;
    o.reconnect_max = 40ms;
    o.invalid_session_wait_min = 0ms;
    o.invalid_session_wait_max = 10ms;
    o.identify_window = 0ms;
    o.close_grace = 500ms;
    o.min_heartbeat_interval = 20ms;
    o.on_dispatch = [&rec](const std::string& t, const Json& d) { rec.add(t, d); };
    return o;
}

Json hello(int interval) { return Json{{"op", 10}, {"d", Json{{"heartbeat_interval", interval}}}}; }
Json dispatch(const std::string& t, std::int64_t s, const Json& d) { return Json{{"op", 0}, {"t", t}, {"s", s}, {"d", d}}; }
Json ready(std::int64_t s, const std::string& resume_url, const std::string& sid = "sess-1") {
    return dispatch("READY", s, Json{{"session_id", sid}, {"resume_gateway_url", resume_url}, {"user", Json{{"id", "1"}}}});
}

/** Hello → identify を受け → READY を返すまで。identify の JSON を返す。 */
Json handshake(FakeConn& c, const std::string& resume_url, int interval = 10'000, std::int64_t s = 1) {
    CHECK(c.send(hello(interval)));
    auto id = c.recv_op(gateway_op::Identify);
    CHECK(id.has_value());
    CHECK(c.send(ready(s, resume_url)));
    return *id;
}

struct LogCapture {
    std::mutex m;
    std::string all;
    int id;
    LogCapture() {
        id = log::add_sink([this](const log::Record& r) {
            std::lock_guard lock(m);
            all += r.message + "\n";
        });
    }
    ~LogCapture() { log::remove_sink(id); }
    std::string text() {
        std::lock_guard lock(m);
        return all;
    }
};
} // namespace

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
