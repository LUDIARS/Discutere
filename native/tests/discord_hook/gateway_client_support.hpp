#pragma once
// gateway_client_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
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

namespace gateway_client_support {
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

    // 読み取りは完了ハンドラで次の読み取りを始めるつなぎ方にする (asio のコルーチンは Praesidium の静的解析が
    // asio 内部で誤検知するため使わない)。
    static void read_next(std::shared_ptr<FakeConn> self) {
        auto buffer = std::make_shared<beast::flat_buffer>();
        self->ws.async_read(*buffer, [self, buffer](boost::system::error_code ec, std::size_t) {
            {
                std::lock_guard lock(self->m);
                if (ec) {
                    self->closed = true;
                    self->close_code = self->ws.reason().code;
                    self->cv.notify_all();
                    return;
                }
                self->frames.push_back(beast::buffers_to_string(buffer->data()));
                self->cv.notify_all();
            }
            read_next(self);
        });
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
        accept_next();
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

    // 接続を 1 本ずつ受ける: TCP の accept → WebSocket の handshake → 次の accept (handshake 失敗は捨てて次へ)。
    void accept_next() {
        acceptor.async_accept([this](boost::system::error_code ec, tcp::socket s) {
            if (ec) return;
            auto conn = std::make_shared<FakeConn>(std::move(s));
            conn->ws.read_message_max(1u << 20);
            conn->ws.async_accept([this, conn](boost::system::error_code ec2) {
                if (!ec2) on_accepted(conn);
                accept_next();
            });
        });
    }

    void on_accepted(const std::shared_ptr<FakeConn>& conn) {
        FakeConn::read_next(conn);
        ++accepted;
        std::lock_guard lock(m);
        all.push_back(conn);
        pending.push_back(conn);
        cv.notify_all();
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
} // namespace gateway_client_support
