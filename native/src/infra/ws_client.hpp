#pragma once
// WebSocket クライアント (Boost.Beast)。Discord Gateway / Slack Socket Mode の土台 (discord.js の ws / ws パッケージの置き換え)。
// - 接続ごとに専用の I/O スレッドを持つ。受信コールバックはその I/O スレッドで呼ばれる
//   (重い処理は background_executor に渡すこと)。
// - 1 メッセージの上限 max_message_bytes (Beast の read_message_max + lorica::input_budget)。
// - 送信はキュー (最大 send_queue_limit 件)。満杯なら send() は false。
// - 圧縮 (permessage-deflate) は使わない。
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "infra/http.hpp"

namespace di::ws {

struct Options {
    std::string url;  // ws:// または wss://
    http::Headers headers;
    std::chrono::milliseconds connect_timeout{15'000};
    std::size_t max_message_bytes = 16u << 20;
    std::size_t send_queue_limit = 1024;
    bool allow_private_network = false;
};

using OnMessage = std::function<void(std::string message)>;
/** 閉じたときに 1 度だけ呼ばれる。code は WebSocket の close code (通信断は 1006、相手が送らなければ 1005)。 */
using OnClose = std::function<void(int code, std::string reason)>;

class Client {
public:
    /** 接続して受信を始める。失敗は nullptr (error に理由)。 */
    static std::shared_ptr<Client> connect(const Options& options, OnMessage on_message, OnClose on_close, std::string* error);
    virtual ~Client() = default;
    virtual bool send(std::string text) = 0;
    virtual void close(int code = 1000, std::string reason = {}) = 0;
    virtual bool is_open() const = 0;
};

} // namespace di::ws
