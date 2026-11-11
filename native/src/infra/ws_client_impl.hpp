#pragma once
// ws_client_plain.cpp / ws_client_tls.cpp だけが include する WebSocket クライアントの本体。
// コールバックの連鎖で進める (asio::awaitable は使わない: Praesidium の静的解析が Boost の
// コルーチン実装の中で誤検知するため。Odeum の relay と同じ書き方)。
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <lorica/input_budget.hpp>

#include "base/log.hpp"
#include "base/url.hpp"
#include "infra/http_client.hpp"
#include "infra/tls.hpp"
#include "infra/ws_client.hpp"

namespace di::ws::detail {

template <class NextLayer>
class Impl final : public Client {
    using tcp = boost::asio::ip::tcp;
    using error_code = boost::system::error_code;

public:
    static constexpr bool kTls = !std::is_same_v<NextLayer, boost::beast::tcp_stream>;

    Impl(Options options, OnMessage on_message, OnClose on_close)
        : options_(std::move(options)), on_message_(std::move(on_message)), on_close_(std::move(on_close)), resolver_(io_) {
        if constexpr (kTls) ws_.emplace(io_, infra::tls_client_context());
        else ws_.emplace(io_);
    }

    // 所有者が最後の参照を手放したら止める。I/O スレッドの中のハンドラは this だけを持つ
    // (shared_ptr を持たない) ので、ここで I/O を止めてスレッドを待ってからメンバを壊す。
    // 受信コールバックの中でこの Client の shared_ptr を強参照で捕まえないこと (循環して止まらない)。
    ~Impl() override {
        stopping_ = true;
        open_ = false;
        work_.reset();
        io_.stop();
        if (thread_.joinable()) {
            if (thread_.get_id() == std::this_thread::get_id()) thread_.detach();
            else thread_.join();
        }
        ws_.reset();
    }

    bool start(const url::Url& u, std::string* error) {
        url_ = u;
        host_header_ = u.host_for_header() + (u.explicit_port ? ":" + std::to_string(u.port) : "");
        work_.emplace(io_.get_executor());
        thread_ = std::thread([this] {
            try {
                io_.run();
            } catch (const std::exception& e) {
                log::error(std::string("[ws] io loop failed: ") + e.what());
            }
        });
        connect_result_ = std::make_shared<std::promise<std::string>>();
        auto future = connect_result_->get_future();
        boost::asio::post(io_, [this] { begin_connect(); });
        if (future.wait_for(options_.connect_timeout + std::chrono::seconds(1)) != std::future_status::ready) {
            boost::asio::post(io_, [this] {
                boost::beast::error_code ec;
                boost::beast::get_lowest_layer(*ws_).socket().close(ec);
            });
            if (error) *error = "connect timeout";
            return false;
        }
        std::string err = future.get();
        if (!err.empty()) {
            if (error) *error = err;
            return false;
        }
        open_ = true;
        boost::asio::post(io_, [this] { read_next(); });
        return true;
    }

    bool send(std::string text) override {
        if (!open_) return false;
        {
            std::lock_guard lock(queue_mutex_);
            if (queued_ >= options_.send_queue_limit) return false;
            ++queued_;
        }
        boost::asio::post(io_, [this, text = std::move(text)]() mutable {
            queue_.push_back(std::move(text));
            if (!writing_) write_next();
        });
        return true;
    }

    void close(int code, std::string reason) override {
        boost::asio::post(io_, [this, code, reason = std::move(reason)] {
            // 2 回目以降の close は何もしない (Beast は async_close の重複を assert で落とす)。
            if (!open_ || closing_) return;
            closing_ = true;
            namespace websocket = boost::beast::websocket;
            websocket::close_reason cr(static_cast<websocket::close_code>(code), reason);
            ws_->async_close(cr, [this](boost::beast::error_code) {
                notify_close(1000, "");
                boost::beast::error_code ec;
                boost::beast::get_lowest_layer(*ws_).socket().close(ec);
            });
        });
    }

    bool is_open() const override { return open_; }

private:
    // --- 接続: 解決 → (SSRF 検査) → TCP → (TLS) → WebSocket ハンドシェイク ---
    void finish_connect(std::string err) {
        if (!connect_result_) return;
        connect_result_->set_value(std::move(err));
        connect_result_.reset();
    }
    void connect_failed(const error_code& ec) {
        std::string err = boost::system::system_error(ec).what();
        finish_connect(err.empty() ? std::string("connect failed") : err);
    }

    void begin_connect() {
        resolver_.async_resolve(url_.host, std::to_string(url_.port),
                                [this](const error_code& ec, const tcp::resolver::results_type& results) { on_resolve(ec, results); });
    }

    void on_resolve(const error_code& ec, const tcp::resolver::results_type& results) {
        if (ec) return connect_failed(ec);
        for (const auto& r : results) {
            if (!options_.allow_private_network && !http::is_public_address(r.endpoint().address().to_string()))
                return finish_connect("blocked: " + url_.host + " resolves to a non-public address");
            endpoints_.push_back(r.endpoint());
        }
        if (endpoints_.empty()) return finish_connect("dns: no address");
        auto& lowest = boost::beast::get_lowest_layer(*ws_);
        lowest.expires_after(options_.connect_timeout);
        lowest.async_connect(endpoints_, [this](const error_code& e, const tcp::endpoint&) { on_tcp_connected(e); });
    }

    void on_tcp_connected(const error_code& ec) {
        if (ec) return connect_failed(ec);
        if constexpr (kTls) {
            namespace ssl = boost::asio::ssl;
            error_code literal;
            boost::asio::ip::make_address(url_.host, literal);
            if (literal && !SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), url_.host.c_str()))
                return finish_connect("tls: SNI failed");
            ws_->next_layer().set_verify_callback(ssl::host_name_verification(url_.host));
            ws_->next_layer().async_handshake(ssl::stream_base::client, [this](const error_code& e) {
                if (e) return connect_failed(e);
                websocket_handshake();
            });
        } else {
            websocket_handshake();
        }
    }

    void websocket_handshake() {
        namespace websocket = boost::beast::websocket;
        boost::beast::get_lowest_layer(*ws_).expires_never();
        ws_->set_option(websocket::stream_base::timeout::suggested(boost::beast::role_type::client));
        auto headers = options_.headers;
        ws_->set_option(websocket::stream_base::decorator([headers](websocket::request_type& req) {
            req.set(boost::beast::http::field::user_agent, "discutere-native/0.1");
            for (const auto& [k, v] : headers.items()) {
                if (k.find_first_of("\r\n") != std::string::npos || v.find_first_of("\r\n") != std::string::npos) continue;
                req.set(k, v);
            }
        }));
        // 圧縮 (permessage-deflate) は交渉しない (Beast の既定と同じだが明示する)。展開後の大きさを
        // 上限で見る経路を持たないため、圧縮されたフレーム (RSV1) は Beast がプロトコル違反として断る。
        websocket::permessage_deflate pmd;
        pmd.client_enable = false;
        pmd.server_enable = false;
        ws_->set_option(pmd);
        // 1 メッセージ (分割フレームをつないだ後) の上限。Beast はフレームの長さを読んだ時点 (確保の前) に断る。
        ws_->read_message_max(options_.max_message_bytes);
        ws_->async_handshake(host_header_, url_.target, [this](const error_code& e) {
            if (e) return connect_failed(e);
            finish_connect("");
        });
    }

    // --- 受信: 1 通ずつ読み、閉じるか上限を超えたら止める ---
    void read_next() {
        read_buffer_.clear();
        ws_->async_read(read_buffer_, [this](const error_code& ec, std::size_t) { on_read(ec); });
    }

    void on_read(const error_code& ec) {
        namespace websocket = boost::beast::websocket;
        if (ec) {
            open_ = false;
            const auto& reason = ws_->reason();
            if (ec == websocket::error::closed) notify_close(reason.code ? reason.code : 1005, std::string(reason.reason.c_str()));
            else notify_close(1006, ec.message());
            return;
        }
        lorica::input_budget budget({options_.max_message_bytes, 1, 1, 1});
        if (!budget.consume(read_buffer_.size(), 1, 1)) {
            open_ = false;
            notify_close(1009, "message too big");
            boost::beast::error_code ignore;
            boost::beast::get_lowest_layer(*ws_).socket().close(ignore);
            return;
        }
        std::string message = boost::beast::buffers_to_string(read_buffer_.data());
        try {
            if (on_message_) on_message_(std::move(message));
        } catch (const std::exception& e) {
            log::error(std::string("[ws] message handler failed: ") + e.what());
        }
        read_next();
    }

    // --- 送信: キューから 1 通ずつ書く ---
    void write_next() {
        if (queue_.empty() || !open_ || closing_) {
            writing_ = false;
            return;
        }
        writing_ = true;
        auto text = std::make_shared<std::string>(std::move(queue_.front()));
        queue_.pop_front();
        {
            std::lock_guard lock(queue_mutex_);
            --queued_;
        }
        ws_->text(true);
        ws_->async_write(boost::asio::buffer(*text), [this, text](boost::beast::error_code ec, std::size_t) {
            if (ec) {
                writing_ = false;
                return;
            }
            write_next();
        });
    }

    void notify_close(int code, std::string reason) {
        open_ = false;
        if (notified_.exchange(true)) return;
        try {
            if (on_close_) on_close_(code, std::move(reason));
        } catch (...) {
        }
    }

    Options options_;
    OnMessage on_message_;
    OnClose on_close_;
    boost::asio::io_context io_;
    tcp::resolver resolver_;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;
    std::optional<boost::beast::websocket::stream<NextLayer>> ws_;
    std::thread thread_;
    url::Url url_;
    std::string host_header_;
    std::vector<tcp::endpoint> endpoints_;
    std::shared_ptr<std::promise<std::string>> connect_result_;
    boost::beast::flat_buffer read_buffer_;
    std::deque<std::string> queue_;
    std::mutex queue_mutex_;
    std::size_t queued_ = 0;
    bool writing_ = false;
    bool closing_ = false;
    std::atomic<bool> open_{false};
    std::atomic<bool> notified_{false};
    std::atomic<bool> stopping_{false};
};

template <class NextLayer>
std::shared_ptr<Client> connect_with(const Options& options, const url::Url& u, OnMessage on_message, OnClose on_close,
                                     std::string* error) {
    auto impl = std::make_shared<Impl<NextLayer>>(options, std::move(on_message), std::move(on_close));
    if (!impl->start(u, error)) return nullptr;
    return impl;
}

} // namespace di::ws::detail
