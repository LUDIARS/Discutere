#pragma once
// socket_mode_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// slack/web_api と slack/socket_mode を偽サーバで確かめる。
// HTTP (apps.connections.open / chat.postMessage / reactions.add) は http::Server、WebSocket は Beast で立てる。
// envelope の即 ack、events_api / interactive の配送、disconnect と切断での再接続、上限超えのメッセージ、接続失敗の上限。
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "check.hpp"
#include "slack/socket_mode.hpp"
#include "slack/web_api.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::slack;
namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

namespace socket_mode_support {

/** 接続ごとに script を実行する偽 Slack WebSocket サーバ。 */
struct FakeWs {
    net::io_context io;
    tcp::acceptor acceptor{io, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0)};
    std::thread thread;
    std::vector<std::thread> workers;  // 接続ごとのスレッド (stop で join する。io より先に終える)
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> acks;
    std::atomic<int> connections{0};
    std::atomic<bool> stopping{false};
    std::function<void(websocket::stream<tcp::socket>&, int)> script;

    unsigned short port() const { return acceptor.local_endpoint().port(); }
    void start() {
        thread = std::thread([this] {
            while (!stopping) {
                tcp::socket sock(io);
                beast::error_code ec;
                acceptor.accept(sock, ec);
                if (ec || stopping) return;
                const int n = ++connections;
                std::lock_guard lock(m);
                workers.emplace_back([this, s = std::move(sock), n]() mutable {
                    try {
                        websocket::stream<tcp::socket> ws(std::move(s));
                        ws.accept();
                        script(ws, n);
                    } catch (...) {
                    }
                });
            }
        });
    }
    void stop() {
        stopping = true;
        // 同期 accept は close では起きないので、自分で 1 本つないで起こす。
        beast::error_code ec;
        net::io_context wake_io;
        tcp::socket wake(wake_io);
        wake.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port()), ec);
        if (thread.joinable()) thread.join();
        std::vector<std::thread> ws;
        {
            std::lock_guard lock(m);
            ws.swap(workers);
        }
        for (auto& w : ws) w.join();
    }
    ~FakeWs() { stop(); }
    /** ack を 1 つ読んで記録する。 */
    void read_ack(websocket::stream<tcp::socket>& ws) {
        beast::flat_buffer buf;
        ws.read(buf);
        std::lock_guard lock(m);
        acks.push_back(beast::buffers_to_string(buf.data()));
        cv.notify_all();
    }
    bool wait_acks(std::size_t n) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, std::chrono::seconds(10), [&] { return acks.size() >= n; });
    }
};

void send(websocket::stream<tcp::socket>& ws, const std::string& text) { ws.write(net::buffer(text)); }

} // namespace socket_mode_support
