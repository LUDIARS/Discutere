#pragma once
// Slack Socket Mode 接続 (公開 URL 不要・常時 WebSocket) — src/slack/socket-mode.ts の移植 (ws パッケージ → di::ws::Client)。
//
// apps.connections.open で wss URL を取得し WebSocket を張る。受信した events_api / interactive /
// slash_commands は即 ack (envelope_id を返す) してからハンドラを呼ぶ (Slack は 3 秒以内の ack が無いと再送する)。
// ハンドラは background_executor で実行する (WS の I/O スレッドを塞がない。キュー満杯なら warn して捨てる)。
// close / 接続失敗のときは指数バックオフ (2s → 最大 30s) で再接続する。stop() まで続けるが、
// 接続に一度も成功しない失敗が max_consecutive_failures 回続いたら諦めて止まる (移植で追加した上限)。
// 1 メッセージの上限 max_message_bytes (WS と JSON の両方で課す)。
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "base/json.hpp"
#include "slack/web_api.hpp"

namespace di::slack {

struct SocketModeDeps {
    /** app-level token (xapp-)。ログに出さない。 */
    std::string appToken;
    /** events_api の payload.event を受ける (message 等)。 */
    std::function<void(const Json& event)> onEvent;
    /** interactive (block_actions 等) の payload を受ける。 */
    std::function<void(const Json& payload)> onInteractive;
    std::function<void(const std::string&)> log;
    std::function<void(const std::string&)> warn;

    WebApiOptions webApi;
    /** 偽サーバ (loopback) を使うテストだけ true。 */
    bool allowPrivateNetwork = false;
    std::size_t maxMessageBytes = 4u << 20;
    std::chrono::milliseconds connectTimeout{15'000};
    std::chrono::milliseconds reconnectBase{2'000};
    std::chrono::milliseconds reconnectMax{30'000};
    /** 接続できない状態が続いたら諦める回数 (0 = 無制限)。 */
    int maxConsecutiveFailures = 100;
    /** false ならハンドラを WS の I/O スレッドで直接呼ぶ (テスト用)。 */
    bool dispatchInBackground = true;
};

class SocketModeHandle {
public:
    virtual ~SocketModeHandle() = default;
    /** 再接続を止めて close する (戻るまで待つ)。 */
    virtual void stop() = 0;
    /** 接続ループが動いているか (諦めた・stop 済みなら false)。 */
    virtual bool running() const = 0;
    /** 今 WebSocket が開いているか。 */
    virtual bool connected() const = 0;
};

/** Socket Mode 接続を開始する (専用スレッドで接続・再接続する)。 */
std::shared_ptr<SocketModeHandle> start_socket_mode(SocketModeDeps deps);

} // namespace di::slack
