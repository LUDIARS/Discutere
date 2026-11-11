// ws::Client の受信の上限 (job cpp-lorica-20261111)。Discord Gateway / Slack Socket Mode の土台なので、
// 偽のサーバから生のフレームを送り、上限ちょうど・上限 + 1・分割フレームの合計・圧縮フレーム・
// 嘘の巨大な長さを確かめる。Gateway は圧縮を使わない (permessage-deflate を交渉しない) ので、
// 圧縮されたフレーム (RSV1) は展開せずに接続ごと断る。
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "infra/ws_client.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace {

constexpr std::size_t kLimit = 64;

/** サーバ → クライアントのフレーム (マスクなし)。first は FIN / RSV / opcode のバイト。 */
std::string frame(unsigned char first, const std::string& payload) {
    std::string out(1, static_cast<char>(first));
    const std::size_t n = payload.size();
    if (n < 126) {
        out.push_back(static_cast<char>(n));
    } else if (n < 65536) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((n >> 8) & 0xFF));
        out.push_back(static_cast<char>(n & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<char>((static_cast<std::uint64_t>(n) >> shift) & 0xFF));
    }
    return out + payload;
}

constexpr unsigned char kFinText = 0x81;
constexpr unsigned char kText = 0x01;          // FIN なし (続きがある)
constexpr unsigned char kFinContinuation = 0x80;
constexpr unsigned char kFinTextRsv1 = 0xC1;  // permessage-deflate で圧縮したことを示す RSV1

/** 1 接続だけ受け、ハンドシェイクの後に生のバイト列を送り、相手が切るまで待つ。 */
struct RawServer {
    asio::io_context io;
    tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
    std::thread thread;
    unsigned short port() const { return acceptor.local_endpoint().port(); }
    void start(std::string bytes) {
        thread = std::thread([this, bytes = std::move(bytes)] {
            try {
                websocket::stream<tcp::socket> ws(acceptor.accept());
                ws.accept();
                asio::write(ws.next_layer(), asio::buffer(bytes));
                // クライアントが何か (プロトコル違反を見つけたときの close フレーム) を送ってきたら、
                // close を返して切る (返さないと Beast は close の応答を待ってから失敗を知らせる)。
                char sink[1024];
                boost::system::error_code ec;
                ws.next_layer().read_some(asio::buffer(sink), ec);
                if (!ec) {
                    const char close_frame[2] = {static_cast<char>(0x88), 0};
                    asio::write(ws.next_layer(), asio::buffer(close_frame, 2), ec);
                    ws.next_layer().shutdown(tcp::socket::shutdown_both, ec);
                }
                while (!ec) ws.next_layer().read_some(asio::buffer(sink), ec);
            } catch (...) {
            }
        });
    }
    ~RawServer() {
        if (thread.joinable()) thread.join();
    }
};

struct Outcome {
    std::vector<std::string> messages;
    int closes = 0;
};

/** bytes を送るサーバに繋ぎ、メッセージか close が来るまで (最大 3 秒) 待つ。 */
Outcome receive(const std::string& bytes) {
    RawServer server;
    server.start(bytes);
    std::mutex m;
    Outcome out;
    di::ws::Options o;
    o.url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/";
    o.allow_private_network = true;
    o.max_message_bytes = kLimit;
    std::string error;
    auto client = di::ws::Client::connect(
        o,
        [&](std::string msg) {
            std::lock_guard lock(m);
            out.messages.push_back(std::move(msg));
        },
        [&](int, std::string) {
            std::lock_guard lock(m);
            ++out.closes;
        },
        &error);
    CHECK(client != nullptr);
    for (int i = 0; i < 150; ++i) {
        {
            std::lock_guard lock(m);
            if (!out.messages.empty() || out.closes > 0) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    client.reset();  // 接続を閉じる → サーバのスレッドが終わる
    std::lock_guard lock(m);
    return out;
}

} // namespace

TEST_CASE("text frame exactly at max_message_bytes is delivered") {
    const auto out = receive(frame(kFinText, std::string(kLimit, 'a')));
    CHECK_EQ(out.messages.size(), std::size_t{1});
    CHECK_EQ(out.messages.front().size(), kLimit);
}

TEST_CASE("text frame of max_message_bytes + 1 closes without delivery") {
    const auto out = receive(frame(kFinText, std::string(kLimit + 1, 'a')));
    CHECK(out.messages.empty());
    CHECK_EQ(out.closes, 1);
}

TEST_CASE("fragments are limited by the reassembled size") {
    // 1 つずつは上限内でも、つないだ後が上限を超えれば渡さない
    const auto over = receive(frame(kText, std::string(40, 'a')) + frame(kFinContinuation, std::string(40, 'b')));
    CHECK(over.messages.empty());
    CHECK_EQ(over.closes, 1);
    const auto exact = receive(frame(kText, std::string(32, 'a')) + frame(kFinContinuation, std::string(32, 'b')));
    CHECK_EQ(exact.messages.size(), std::size_t{1});
    CHECK_EQ(exact.messages.front(), std::string(32, 'a') + std::string(32, 'b'));
}

TEST_CASE("compressed (RSV1) frame is refused without inflating") {
    // "aaaa..." を deflate した短いバイト列 (展開すると上限を大きく超える形)。permessage-deflate は交渉して
    // いないので、展開せずにプロトコル違反として閉じる。
    const std::string deflated("\x4b\x4c\x1c\x05\xa3\x60\x14\x0c\x77\x00\x00", 11);
    const auto out = receive(frame(kFinTextRsv1, deflated));
    CHECK(out.messages.empty());
    CHECK_EQ(out.closes, 1);
}

TEST_CASE("a header that claims a huge length is refused before allocating") {
    // 64 ビットの長さ 2^62 を名乗るだけで本文は送らない。read_message_max で確保の前に断る。
    std::string header;
    header.push_back(static_cast<char>(kFinText));
    header.push_back(static_cast<char>(127));
    header.push_back(static_cast<char>(0x40));
    header.append(7, '\0');
    const auto out = receive(header);
    CHECK(out.messages.empty());
    CHECK_EQ(out.closes, 1);
}

TEST_CASE("broken frame (reserved opcode) closes the connection") {
    const auto out = receive(frame(0x83, "x"));  // opcode 3 は予約
    CHECK(out.messages.empty());
    CHECK_EQ(out.closes, 1);
}

DI_TEST_MAIN()
