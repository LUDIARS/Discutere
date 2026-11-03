#pragma once
// w5-U: 偽の Discord Gateway (Beast の WebSocket サーバ)。tests/discord_hook/gateway_client_tests.cpp の FakeConn / FakeGateway と同じもの。
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "base/json.hpp"

namespace w5u {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;
using di::Json;
using di::JsonLimits;
using Clock = std::chrono::steady_clock;

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
        return di::parse_json(text, limits);
    }

    /** op が一致するフレームまで読み飛ばす (heartbeat が挟まるため)。 */
    std::optional<Json> recv_op(int op, std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            auto f = recv(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
            if (!f) return std::nullopt;
            if (di::json_int(*f, "op", -1) == op) return f;
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
    bool send(const Json& j) { return send_text(di::json_stringify(j)); }

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

} // namespace w5u
