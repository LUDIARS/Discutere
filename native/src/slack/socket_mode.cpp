#include "slack/socket_mode.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "base/executor.hpp"
#include "base/log.hpp"
#include "infra/ws_client.hpp"

namespace di::slack {

namespace {

/** 1 本の WebSocket 接続の状態 (コールバックと接続ループが共有する)。 */
struct Connection {
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<ws::Client> client;
    bool closed = false;
    bool abandoned = false;
    /** close() は 1 回だけ呼ぶ (ws::Client::close を重ねて呼ぶと Beast が close_op の重複で落ちる)。 */
    std::atomic<bool> close_requested{false};
    void close_once() {
        if (close_requested.exchange(true)) return;
        std::shared_ptr<ws::Client> c;
        {
            std::lock_guard lock(mutex);
            c = client;
        }
        if (c) c->close();
    }
};

class SocketMode final : public SocketModeHandle, public std::enable_shared_from_this<SocketMode> {
public:
    explicit SocketMode(SocketModeDeps deps) : deps_(std::move(deps)) {
        if (!deps_.log) deps_.log = [](const std::string& m) { log::info("  [slack-socket] " + m); };
        if (!deps_.warn) deps_.warn = [](const std::string& m) { log::warn("  [slack-socket/warn] " + m); };
    }
    ~SocketMode() override { stop(); }

    void start() {
        running_ = true;
        thread_ = std::thread([self = shared_from_this()] { self->loop(); });
    }

    void stop() override {
        std::shared_ptr<Connection> conn;
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
            conn = current_;
        }
        cv_.notify_all();
        if (conn) {
            {
                std::lock_guard lock(conn->mutex);
                conn->abandoned = true;
            }
            conn->cv.notify_all();
            conn->close_once();
        }
        if (thread_.joinable()) {
            // 最後の参照が接続ループ自身で外れたときは自分を join できないので切り離す。
            if (thread_.get_id() == std::this_thread::get_id()) thread_.detach();
            else thread_.join();
        }
        running_ = false;
    }

    bool running() const override { return running_; }
    bool connected() const override {
        std::lock_guard lock(mutex_);
        if (!current_) return false;
        std::lock_guard c(current_->mutex);
        return current_->client && !current_->closed && current_->client->is_open();
    }

private:
    bool is_stopped() {
        std::lock_guard lock(mutex_);
        return stopped_;
    }

    /** 停止要求で起きたら false。 */
    bool wait(std::chrono::milliseconds d) {
        std::unique_lock lock(mutex_);
        return !cv_.wait_for(lock, d, [&] { return stopped_; });
    }

    void loop() {
        int attempt = 0;
        int failures = 0;
        bool first = true;
        while (!is_stopped()) {
            if (!first) {
                const long long base = deps_.reconnectBase.count();
                const long long delay = std::min<long long>(base << std::min(attempt, 20), deps_.reconnectMax.count());
                attempt += 1;
                if (!wait(std::chrono::milliseconds(delay))) break;
            }
            first = false;
            if (deps_.maxConsecutiveFailures > 0 && failures >= deps_.maxConsecutiveFailures) {
                log::error("  [slack-socket] 接続失敗が " + std::to_string(failures) + " 回続いたので再接続を止める");
                break;
            }
            std::string url;
            try {
                url = apps_connections_open(deps_.appToken, deps_.webApi);
            } catch (const std::exception& e) {
                deps_.warn(std::string("接続 URL 取得失敗: ") + e.what() + " (再試行)");
                ++failures;
                continue;
            }
            if (is_stopped()) break;

            auto conn = std::make_shared<Connection>();
            ws::Options options;
            options.url = url;
            options.connect_timeout = deps_.connectTimeout;
            options.max_message_bytes = deps_.maxMessageBytes;
            options.allow_private_network = deps_.allowPrivateNetwork;
            std::weak_ptr<SocketMode> weak = shared_from_this();
            std::string error;
            auto client = ws::Client::connect(
                options,
                [weak, conn](std::string message) {
                    if (auto self = weak.lock()) self->handle_message(*conn, message);
                },
                [conn](int, std::string) {
                    {
                        std::lock_guard lock(conn->mutex);
                        conn->closed = true;
                    }
                    conn->cv.notify_all();
                },
                &error);
            if (!client) {
                deps_.warn("WebSocket 接続失敗: " + error);
                ++failures;
                continue;
            }
            {
                std::lock_guard lock(conn->mutex);
                conn->client = client;
            }
            conn->cv.notify_all();
            {
                std::lock_guard lock(mutex_);
                current_ = conn;
            }
            attempt = 0;  // 成功でバックオフをリセット
            failures = 0;
            deps_.log("WebSocket 接続");

            {
                std::unique_lock lock(conn->mutex);
                conn->cv.wait(lock, [&] { return conn->closed || conn->abandoned; });
            }
            {
                std::lock_guard lock(mutex_);
                if (current_ == conn) current_.reset();
            }
            if (is_stopped()) {
                conn->close_once();
                break;
            }
            deps_.warn("WebSocket close → 再接続をスケジュール");
        }
        running_ = false;
    }

    /** 受信 envelope に envelope_id があれば即 ack を返す (最優先)。 */
    void ack(Connection& conn, const Json& envelope) {
        auto id = json_opt_string(envelope, "envelope_id");
        if (!id || id->empty()) return;
        std::shared_ptr<ws::Client> client;
        {
            // connect() が戻る前に届いたメッセージは、client が入るまで少し待つ。
            std::unique_lock lock(conn.mutex);
            conn.cv.wait_for(lock, std::chrono::seconds(5), [&] { return conn.client != nullptr || conn.abandoned; });
            client = conn.client;
        }
        if (!client || !client->is_open()) return;
        if (!client->send(json_stringify(Json{{"envelope_id", *id}}))) deps_.warn("ack 送信失敗: send queue full");
    }

    /** payload が文字列なら JSON として読み、object ならそのまま。 */
    std::optional<Json> as_payload(const Json& envelope) {
        if (!envelope.contains("payload")) return std::nullopt;
        const Json& raw = envelope["payload"];
        if (raw.is_object()) return raw;
        if (raw.is_string()) {
            JsonLimits limits;
            limits.max_bytes = deps_.maxMessageBytes;
            auto parsed = parse_json(raw.get<std::string>(), limits);
            if (parsed && parsed->is_object()) return parsed;
        }
        return std::nullopt;
    }

    void dispatch(std::function<void()> fn, const char* what) {
        auto guarded = [fn = std::move(fn), warn = deps_.warn, what] {
            try {
                fn();
            } catch (const std::exception& e) {
                warn(std::string(what) + " 例外: " + e.what());
            }
        };
        if (!deps_.dispatchInBackground) {
            guarded();
            return;
        }
        if (!background_executor().submit(guarded)) deps_.warn(std::string(what) + " を捨てた (背景キュー満杯)");
    }

    void handle_message(Connection& conn, const std::string& data) {
        JsonLimits limits;
        limits.max_bytes = deps_.maxMessageBytes;
        auto parsed = parse_json(data, limits);
        if (!parsed || !parsed->is_object()) return;  // 非 JSON は無視
        const Json& envelope = *parsed;
        const std::string type = json_string(envelope, "type");
        if (type == "hello") {
            deps_.log("hello (接続確立)");
        } else if (type == "disconnect") {
            deps_.log("disconnect 受信 → 再接続");
            // close ハンドラが接続ループを起こし、再接続をスケジュールする。
            conn.close_once();
        } else if (type == "events_api") {
            ack(conn, envelope);
            auto payload = as_payload(envelope);
            if (payload && payload->contains("event") && (*payload)["event"].is_object() && deps_.onEvent) {
                Json event = (*payload)["event"];
                dispatch([fn = deps_.onEvent, event = std::move(event)] { fn(event); }, "onEvent");
            }
        } else if (type == "interactive") {
            ack(conn, envelope);
            auto payload = as_payload(envelope);
            if (payload && deps_.onInteractive)
                dispatch([fn = deps_.onInteractive, p = std::move(*payload)] { fn(p); }, "onInteractive");
        } else if (type == "slash_commands") {
            ack(conn, envelope);  // 今回は未使用 (ack のみ)
        }
    }

    SocketModeDeps deps_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_ = false;
    std::atomic<bool> running_{false};
    std::shared_ptr<Connection> current_;
    std::thread thread_;
};

} // namespace

std::shared_ptr<SocketModeHandle> start_socket_mode(SocketModeDeps deps) {
    auto sm = std::make_shared<SocketMode>(std::move(deps));
    sm->start();
    return sm;
}

} // namespace di::slack
