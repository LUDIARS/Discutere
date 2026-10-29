#pragma once
// HTTP/1.1 サーバ (Boost.Beast)。@hono/node-server の置き換え。
// - 受付と読み書きは I/O スレッド (既定 2 本) で非同期に行い、ハンドラは上限つきのプールで実行する。
// - ヘッダ 16KiB、本文はルートごとの上限 (Router::max_body_for)。超えたら 413。
// - 期限: ヘッダ 30 秒、本文 60 秒、keep-alive の待ち 30 秒、書き込み 60 秒。
// - 同時接続 512 まで。超えた接続はすぐ閉じる。ハンドラのキューが満杯なら 503。
#include <cstdint>
#include <memory>
#include <string>

#include "infra/http.hpp"

namespace di::http {

struct ServerOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3110;
    unsigned io_threads = 2;
    unsigned handler_threads = 16;
    std::size_t handler_queue = 512;
    std::size_t max_connections = 512;
    std::size_t max_header_bytes = 16 * 1024;
};

class Server {
public:
    Server(ServerOptions options, std::shared_ptr<const Router> router);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    /** bind して受付を始める (戻る)。失敗は std::runtime_error。 */
    void start();
    /** 実際に bind したポート (port=0 のテスト用)。 */
    std::uint16_t bound_port() const;
    void stop();

    struct Impl;  // 実装 (http_server.cpp)

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace di::http
