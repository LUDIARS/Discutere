#include "infra/ws_client.hpp"

#include <atomic>
#include <deque>
#include <future>
#include <optional>
#include <thread>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <lorica/input_budget.hpp>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/url.hpp"
#include "infra/http_client.hpp"
#include "infra/tls.hpp"

namespace di::ws {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = boost::beast::websocket;
namespace ssl = boost::asio::ssl;
using tcp = asio::ip::tcp;

namespace {

template <class NextLayer>
class Impl final : public Client {
public:
    static constexpr bool kTls = !std::is_same_v<NextLayer, beast::tcp_stream>;

    Impl(Options options, OnMessage on_message, OnClose on_close)
        : options_(std::move(options)), on_message_(std::move(on_message)), on_close_(std::move(on_close)) {
        if constexpr (kTls) ws_.emplace(io_, infra::tls_client_context());
        else ws_.emplace(io_);
    }

    // 所有者が最後の参照を手放したら止める。I/O スレッドの中のコルーチンは this だけを持つ
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
        work_.emplace(io_.get_executor());
        thread_ = std::thread([this] {
            try {
                io_.run();
            } catch (const std::exception& e) {
                log::error(std::string("[ws] io loop failed: ") + e.what());
            }
        });
        auto promise = std::make_shared<std::promise<std::string>>();
        auto future = promise->get_future();
        asio::co_spawn(io_, connect(u), [promise](std::exception_ptr e) {
            std::string err;
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    err = ex.what();
                    if (err.empty()) err = "connect failed";
                }
            }
            promise->set_value(err);
        });
        if (future.wait_for(options_.connect_timeout + std::chrono::seconds(1)) != std::future_status::ready) {
            asio::post(io_, [this] {
                beast::error_code ec;
                beast::get_lowest_layer(*ws_).socket().close(ec);
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
        asio::co_spawn(io_, read_loop(), asio::detached);
        return true;
    }

    bool send(std::string text) override {
        if (!open_) return false;
        {
            std::lock_guard lock(queue_mutex_);
            if (queued_ >= options_.send_queue_limit) return false;
            ++queued_;
        }
        asio::post(io_, [this, text = std::move(text)]() mutable {
            queue_.push_back(std::move(text));
            if (!writing_) write_next();
        });
        return true;
    }

    void close(int code, std::string reason) override {
        asio::post(io_, [this, code, reason = std::move(reason)] {
            if (!open_) return;
            closing_ = true;
            websocket::close_reason cr(static_cast<websocket::close_code>(code), reason);
            ws_->async_close(cr, [this](beast::error_code) {
                notify_close(1000, "");
                beast::error_code ec;
                beast::get_lowest_layer(*ws_).socket().close(ec);
            });
        });
    }

    bool is_open() const override { return open_; }

private:
    asio::awaitable<void> connect(url::Url u) {
        auto ex = co_await asio::this_coro::executor;
        tcp::resolver resolver(ex);
        auto results = co_await resolver.async_resolve(u.host, std::to_string(u.port), asio::use_awaitable);
        std::vector<tcp::endpoint> endpoints;
        for (const auto& r : results) {
            if (!options_.allow_private_network && !http::is_public_address(r.endpoint().address().to_string()))
                throw std::runtime_error("blocked: " + u.host + " resolves to a non-public address");
            endpoints.push_back(r.endpoint());
        }
        if (endpoints.empty()) throw std::runtime_error("dns: no address");
        auto& lowest = beast::get_lowest_layer(*ws_);
        lowest.expires_after(options_.connect_timeout);
        co_await lowest.async_connect(endpoints, asio::use_awaitable);
        if constexpr (kTls) {
            boost::system::error_code ec;
            asio::ip::make_address(u.host, ec);
            if (ec && !SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), u.host.c_str()))
                throw std::runtime_error("tls: SNI failed");
            ws_->next_layer().set_verify_callback(ssl::host_name_verification(u.host));
            co_await ws_->next_layer().async_handshake(ssl::stream_base::client, asio::use_awaitable);
        }
        lowest.expires_never();
        ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        auto headers = options_.headers;
        ws_->set_option(websocket::stream_base::decorator([headers](websocket::request_type& req) {
            req.set(beast::http::field::user_agent, "discutere-native/0.1");
            for (const auto& [k, v] : headers.items()) {
                if (k.find_first_of("\r\n") != std::string::npos || v.find_first_of("\r\n") != std::string::npos) continue;
                req.set(k, v);
            }
        }));
        ws_->read_message_max(options_.max_message_bytes);
        const std::string host = u.host_for_header() + (u.explicit_port ? ":" + std::to_string(u.port) : "");
        co_await ws_->async_handshake(host, u.target, asio::use_awaitable);
    }

    asio::awaitable<void> read_loop() {
        while (true) {
            beast::flat_buffer buffer;
            boost::system::error_code ec;
            co_await ws_->async_read(buffer, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                open_ = false;
                const auto& reason = ws_->reason();
                if (ec == websocket::error::closed) notify_close(reason.code ? reason.code : 1005, std::string(reason.reason.c_str()));
                else notify_close(1006, ec.message());
                co_return;
            }
            lorica::input_budget budget({options_.max_message_bytes, 1, 1, 1});
            if (!budget.consume(buffer.size(), 1, 1)) {
                open_ = false;
                notify_close(1009, "message too big");
                beast::error_code ignore;
                beast::get_lowest_layer(*ws_).socket().close(ignore);
                co_return;
            }
            std::string message = beast::buffers_to_string(buffer.data());
            try {
                if (on_message_) on_message_(std::move(message));
            } catch (const std::exception& e) {
                log::error(std::string("[ws] message handler failed: ") + e.what());
            }
        }
    }

    void write_next() {
        if (queue_.empty() || !open_) {
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
        ws_->async_write(asio::buffer(*text), [this, text](beast::error_code ec, std::size_t) {
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
    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::optional<websocket::stream<NextLayer>> ws_;
    std::thread thread_;
    std::deque<std::string> queue_;
    std::mutex queue_mutex_;
    std::size_t queued_ = 0;
    bool writing_ = false;
    bool closing_ = false;
    std::atomic<bool> open_{false};
    std::atomic<bool> notified_{false};
    std::atomic<bool> stopping_{false};
};

} // namespace

std::shared_ptr<Client> Client::connect(const Options& options, OnMessage on_message, OnClose on_close, std::string* error) {
    auto u = url::parse(options.url);
    if (!u || (u->scheme != "ws" && u->scheme != "wss")) {
        if (error) *error = "invalid websocket url";
        return nullptr;
    }
    if (u->scheme == "wss") {
        auto impl = std::make_shared<Impl<beast::ssl_stream<beast::tcp_stream>>>(options, std::move(on_message), std::move(on_close));
        if (!impl->start(*u, error)) return nullptr;
        return impl;
    }
    auto impl = std::make_shared<Impl<beast::tcp_stream>>(options, std::move(on_message), std::move(on_close));
    if (!impl->start(*u, error)) return nullptr;
    return impl;
}

} // namespace di::ws
