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

namespace {

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

} // namespace

TEST_CASE("web api: bearer, truncation, errors") {
    w1d::FakeServer fake;
    std::mutex m;
    Json last;
    std::string auth;
    fake.router->post("/chat.postMessage", [&](http::Request& r) {
        std::lock_guard lock(m);
        last = Json::parse(r.body);
        auth = r.header("Authorization").value_or("");
        if (last["channel"] == "bad") return http::Response::json(Json{{"ok", false}, {"error", "channel_not_found"}});
        return http::Response::json(Json{{"ok", true}, {"ts", "1.2"}});
    });
    fake.router->post("/reactions.add", [&](http::Request&) { return http::Response::json(Json{{"ok", false}, {"error", "already_reacted"}}); });
    fake.router->post("/apps.connections.open", [](http::Request& r) {
        if (r.header("Authorization") != std::optional<std::string>("Bearer xapp-fake")) return http::Response::json(Json{{"ok", false}, {"error", "invalid_auth"}});
        return http::Response::text("<html>not json</html>");
    });
    fake.start();
    WebApiOptions o;
    o.base_url = fake.base;
    o.allow_private_network = true;
    auto posted = post_message("xoxb-fake", {"C1", std::string(4000, 'a'), std::string("9.9"), std::nullopt, std::nullopt, std::nullopt}, o);
    CHECK_EQ(posted.ts, std::string("1.2"));
    CHECK_EQ(posted.channel, std::string("C1"));
    CHECK_EQ(auth, std::string("Bearer xoxb-fake"));
    CHECK_EQ(str::js_length(last["text"].get<std::string>()), std::size_t(3000));
    CHECK_EQ(last["thread_ts"].get<std::string>(), std::string("9.9"));
    CHECK(!last.contains("username"));
    try {
        post_message("xoxb-fake", {"bad", "x", std::nullopt, std::nullopt, std::nullopt, std::nullopt}, o);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("chat.postMessage 失敗: channel_not_found"));
        CHECK(!str::contains(e.what(), "xoxb-fake"));
    }
    reactions_add("xoxb-fake", {"C1", "1.2", "trophy"}, o);  // already_reacted は投げない
    try {
        apps_connections_open("xapp-fake", o);  // 本文が JSON でない → invalid_json
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("apps.connections.open 失敗: invalid_json"));
    }
    CHECK_EQ(truncate_slack("abc", 3), std::string("abc"));
    CHECK_EQ(truncate_slack("abcd", 3), std::string("ab…"));
    // 既定 (allow_private_network=false) は loopback に出ない
    WebApiOptions d;
    d.base_url = fake.base;
    CHECK_THROWS(post_message("xoxb-fake", {"C1", "x", std::nullopt, std::nullopt, std::nullopt, std::nullopt}, d));
}

TEST_CASE("socket mode: ack first, dispatch, reconnect on disconnect, oversized frame") {
    FakeWs ws_server;
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> events, interactives;
    ws_server.script = [&](websocket::stream<tcp::socket>& ws, int n) {
        send(ws, R"({"type":"hello"})");
        if (n == 1) {
            send(ws, "not json");
            send(ws, R"({"type":"events_api","envelope_id":"e1","payload":{"event":{"type":"message","text":"テーマ"}}})");
            ws_server.read_ack(ws);
            send(ws, R"({"type":"interactive","envelope_id":"e2","payload":"{\"type\":\"block_actions\"}"})");
            ws_server.read_ack(ws);
            send(ws, R"({"type":"slash_commands","envelope_id":"e3"})");
            ws_server.read_ack(ws);
            send(ws, R"({"type":"disconnect","reason":"refresh_requested"})");
            beast::flat_buffer buf;
            beast::error_code ec;
            ws.read(buf, ec);  // クライアントの close を待つ
            return;
        }
        if (n == 2) {
            // 上限を超えるメッセージ → クライアントが切断 → 再接続
            send(ws, std::string(64 * 1024 + 1, 'x'));
            beast::flat_buffer buf;
            beast::error_code ec;
            ws.read(buf, ec);
            return;
        }
        send(ws, R"({"type":"events_api","envelope_id":"e4","payload":{"event":{"type":"message","text":"再接続後"}}})");
        ws_server.read_ack(ws);
        beast::flat_buffer buf;
        beast::error_code ec;
        ws.read(buf, ec);
    };
    ws_server.start();

    w1d::FakeServer http_fake;
    std::atomic<int> opens{0};
    http_fake.router->post("/apps.connections.open", [&](http::Request& r) {
        ++opens;
        CHECK_EQ(*r.header("Authorization"), std::string("Bearer xapp-fake"));
        return http::Response::json(Json{{"ok", true}, {"url", "ws://127.0.0.1:" + std::to_string(ws_server.port()) + "/link"}});
    });
    http_fake.start();

    SocketModeDeps deps;
    deps.appToken = "xapp-fake";
    deps.webApi.base_url = http_fake.base;
    deps.webApi.allow_private_network = true;
    deps.allowPrivateNetwork = true;
    deps.maxMessageBytes = 64 * 1024;
    deps.reconnectBase = std::chrono::milliseconds(20);
    deps.reconnectMax = std::chrono::milliseconds(100);
    deps.onEvent = [&](const Json& e) {
        std::lock_guard lock(m);
        events.push_back(e["text"].get<std::string>());
        cv.notify_all();
    };
    deps.onInteractive = [&](const Json& p) {
        std::lock_guard lock(m);
        interactives.push_back(p["type"].get<std::string>());
        cv.notify_all();
    };
    auto handle = start_socket_mode(deps);
    CHECK(ws_server.wait_acks(4));
    {
        std::unique_lock lock(m);
        CHECK(cv.wait_for(lock, std::chrono::seconds(10), [&] { return events.size() >= 2 && interactives.size() >= 1; }));
    }
    handle->stop();
    ws_server.stop();
    CHECK(!handle->running());
    CHECK_EQ(ws_server.acks[0], std::string(R"({"envelope_id":"e1"})"));
    CHECK_EQ(ws_server.acks[1], std::string(R"({"envelope_id":"e2"})"));
    CHECK_EQ(ws_server.acks[2], std::string(R"({"envelope_id":"e3"})"));
    CHECK_EQ(ws_server.acks[3], std::string(R"({"envelope_id":"e4"})"));
    CHECK_EQ(events[0], std::string("テーマ"));
    CHECK_EQ(events[1], std::string("再接続後"));
    CHECK_EQ(interactives[0], std::string("block_actions"));
    CHECK(ws_server.connections.load() >= 3);
    CHECK(opens.load() >= 3);
}

TEST_CASE("socket mode gives up after consecutive failures and never logs the token") {
    std::mutex m;
    std::string logs;
    const int sink = log::add_sink([&](const log::Record& r) {
        std::lock_guard lock(m);
        logs += r.message + "\n";
    });
    w1d::FakeServer http_fake;
    std::atomic<int> opens{0};
    http_fake.router->post("/apps.connections.open", [&](http::Request&) {
        ++opens;
        return http::Response::json(Json{{"ok", false}, {"error", "invalid_auth"}});
    });
    http_fake.start();
    SocketModeDeps deps;
    deps.appToken = "xapp-secret-should-not-leak";
    deps.webApi.base_url = http_fake.base;
    deps.webApi.allow_private_network = true;
    deps.reconnectBase = std::chrono::milliseconds(5);
    deps.reconnectMax = std::chrono::milliseconds(10);
    deps.maxConsecutiveFailures = 3;
    auto handle = start_socket_mode(deps);
    for (int i = 0; i < 200 && handle->running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!handle->running());
    CHECK_EQ(opens.load(), 3);
    handle->stop();
    log::remove_sink(sink);
    CHECK(!str::contains(logs, "xapp-secret-should-not-leak"));
    CHECK(str::contains(logs, "invalid_auth"));
}

DI_TEST_MAIN()
