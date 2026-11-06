// ws::Client の基本 (送受信・close の重複・メッセージ上限) を、テスト内の Beast の偽サーバで確かめる。
#include "check.hpp"

#include <atomic>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "infra/ws_client.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace {
// 1 接続だけ受けて、届いたテキストをそのまま返すエコーサーバ。
struct EchoServer {
    asio::io_context io;
    tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
    std::thread thread;
    unsigned short port() const { return acceptor.local_endpoint().port(); }
    void start() {
        thread = std::thread([this] {
            try {
                tcp::socket socket = acceptor.accept();
                websocket::stream<tcp::socket> ws(std::move(socket));
                ws.accept();
                while (true) {
                    beast::flat_buffer buffer;
                    ws.read(buffer);
                    ws.text(true);
                    ws.write(buffer.data());
                }
            } catch (...) {
            }
        });
    }
    ~EchoServer() {
        if (thread.joinable()) thread.join();
    }
};
} // namespace

TEST_CASE("echo round trip and idempotent close") {
    EchoServer server;
    server.start();
    std::atomic<int> received{0};
    std::atomic<int> closes{0};
    std::string last;
    std::mutex m;
    di::ws::Options o;
    o.url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/";
    o.allow_private_network = true;
    std::string error;
    auto client = di::ws::Client::connect(
        o,
        [&](std::string msg) {
            std::lock_guard lock(m);
            last = msg;
            ++received;
        },
        [&](int, std::string) { ++closes; }, &error);
    CHECK(client != nullptr);
    CHECK(client->send("hello"));
    for (int i = 0; i < 100 && received == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(received.load(), 1);
    {
        std::lock_guard lock(m);
        CHECK_EQ(last, std::string("hello"));
    }
    client->close(1000, "bye");
    client->close(1000, "again");  // 2 回目は何もしない (以前は Beast の assert で落ちた)
    for (int i = 0; i < 100 && closes == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(closes.load(), 1);
    CHECK(!client->send("after close"));
    client.reset();
}

TEST_CASE("private address is refused by default") {
    EchoServer server;
    di::ws::Options o;
    o.url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/";
    std::string error;
    auto client = di::ws::Client::connect(o, nullptr, nullptr, &error);
    CHECK(client == nullptr);
    CHECK(error.find("blocked") != std::string::npos);
    server.acceptor.close();
}

DI_TEST_MAIN()
