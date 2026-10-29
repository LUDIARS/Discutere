#include "infra/http_server.hpp"

#include <atomic>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <lorica/input_budget.hpp>

#include "base/executor.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"

namespace di::http {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = boost::beast::http;
using tcp = asio::ip::tcp;

namespace {
constexpr auto kHeaderTimeout = std::chrono::seconds(30);
constexpr auto kBodyTimeout = std::chrono::seconds(60);
constexpr auto kWriteTimeout = std::chrono::seconds(60);
} // namespace

struct Server::Impl {
    ServerOptions options;
    std::shared_ptr<const Router> router;
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    tcp::acceptor acceptor{io};
    std::vector<std::thread> threads;
    std::unique_ptr<Executor> handlers;
    std::atomic<std::size_t> connections{0};
    std::atomic<bool> running{false};
    std::uint16_t port = 0;

    void accept();
};

namespace {

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, Server::Impl& server) : stream_(std::move(socket)), server_(server) {
        beast::error_code ec;
        auto ep = stream_.socket().remote_endpoint(ec);
        if (!ec) {
            auto addr = ep.address();
            remote_ = addr.to_string();
        }
    }
    ~Session() { server_.connections.fetch_sub(1); }

    void start() { read_header(); }

private:
    void read_header() {
        parser_.emplace();
        parser_->header_limit(static_cast<std::uint32_t>(server_.options.max_header_bytes));
        // ヘッダを読む間は本文の上限を外す (ヘッダ読み込み時に Content-Length で弾かれないように)。
        // ルートが決まった on_header で本当の上限を課す。
        parser_->body_limit(std::numeric_limits<std::uint64_t>::max());
        stream_.expires_after(kHeaderTimeout);
        bhttp::async_read_header(stream_, buffer_, *parser_,
                                 [self = shared_from_this()](beast::error_code ec, std::size_t) { self->on_header(ec); });
    }

    void on_header(beast::error_code ec) {
        if (ec) return close();
        const auto& req = parser_->get();
        const std::string target(req.target());
        const std::string path = target.substr(0, target.find('?'));
        const std::size_t limit = server_.router->max_body_for(std::string(req.method_string()), path);
        const auto length = parser_->content_length();
        if (length && *length > limit) return respond_error(413, "Payload Too Large", false);
        parser_->body_limit(limit);
        stream_.expires_after(kBodyTimeout);
        bhttp::async_read(stream_, buffer_, *parser_, [self = shared_from_this()](beast::error_code e, std::size_t) {
            self->on_body(e);
        });
    }

    void on_body(beast::error_code ec) {
        if (ec == bhttp::error::body_limit) return respond_error(413, "Payload Too Large", false);
        if (ec) return close();
        auto req = parser_->release();
        keep_alive_ = req.keep_alive();
        auto request = std::make_shared<Request>();
        request->method = std::string(req.method_string());
        request->target = std::string(req.target());
        const auto q = request->target.find('?');
        request->path = request->target.substr(0, q);
        if (q != std::string::npos) request->query_string = request->target.substr(q + 1);
        for (const auto& field : req) request->headers.add(std::string(field.name_string()), std::string(field.value()));
        request->body = std::move(req.body());
        request->remote_address = remote_;
        // 本文の課金 (Beast の body_limit に加えて明示的に)。
        lorica::input_budget budget({server_.router->max_body_for(request->method, request->path), 1, 1, 1});
        if (!budget.consume(request->body.size(), 1, 1)) return respond_error(413, "Payload Too Large", false);

        auto self = shared_from_this();
        const bool queued = server_.handlers->submit([self, request] {
            Response response;
            try {
                response = self->server_.router->handle(*request);
            } catch (const std::exception& e) {
                log::error(std::string("[http] handler failed: ") + e.what());
                response = Response::text("Internal Server Error", 500);
            } catch (...) {
                response = Response::text("Internal Server Error", 500);
            }
            auto shared = std::make_shared<Response>(std::move(response));
            asio::post(self->stream_.get_executor(), [self, shared, head = request->method == "HEAD"] {
                self->write(*shared, head);
            });
        });
        if (!queued) respond_error(503, "Service Unavailable", false);
    }

    void respond_error(int status, const char* text, bool keep) {
        Response r = Response::text(text, status);
        keep_alive_ = keep;
        write(r, false);
    }

    void write(const Response& r, bool head) {
        auto res = std::make_shared<bhttp::response<bhttp::string_body>>();
        res->version(11);
        res->result(static_cast<unsigned>(r.status));
        for (const auto& [k, v] : r.headers.items()) {
            // 応答ヘッダへの改行注入を防ぐ。
            if (k.find_first_of("\r\n") != std::string::npos || v.find_first_of("\r\n") != std::string::npos) continue;
            res->insert(k, v);
        }
        if (head) {
            res->content_length(r.body.size());
        } else {
            res->body() = r.body;
            res->prepare_payload();
        }
        res->keep_alive(keep_alive_);
        stream_.expires_after(kWriteTimeout);
        bhttp::async_write(stream_, *res, [self = shared_from_this(), res](beast::error_code ec, std::size_t) {
            if (ec || !self->keep_alive_ || !self->server_.running) return self->close();
            self->read_header();
        });
    }

    void close() {
        beast::error_code ec;
        stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
        stream_.socket().close(ec);
    }

    beast::tcp_stream stream_;
    Server::Impl& server_;
    beast::flat_buffer buffer_;
    std::optional<bhttp::request_parser<bhttp::string_body>> parser_;
    std::string remote_;
    bool keep_alive_ = false;
};

} // namespace

void Server::Impl::accept() {
    acceptor.async_accept(asio::make_strand(io), [this](beast::error_code ec, tcp::socket socket) {
        if (!running) return;
        if (!ec) {
            if (connections.fetch_add(1) >= options.max_connections) {
                connections.fetch_sub(1);
                beast::error_code ignore;
                socket.close(ignore);
            } else {
                std::make_shared<Session>(std::move(socket), *this)->start();
            }
        }
        accept();
    });
}

Server::Server(ServerOptions options, std::shared_ptr<const Router> router) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    impl_->router = std::move(router);
}

Server::~Server() { stop(); }

void Server::start() {
    auto& im = *impl_;
    im.handlers = std::make_unique<Executor>("http", im.options.handler_threads, im.options.handler_queue);
    beast::error_code ec;
    asio::ip::address address = asio::ip::make_address(im.options.host, ec);
    if (ec) {
        tcp::resolver resolver(im.io);
        auto results = resolver.resolve(im.options.host, std::to_string(im.options.port), ec);
        if (ec || results.empty()) throw std::runtime_error("http: cannot resolve bind host " + im.options.host);
        address = results.begin()->endpoint().address();
    }
    tcp::endpoint endpoint(address, im.options.port);
    im.acceptor.open(endpoint.protocol());
    im.acceptor.set_option(asio::socket_base::reuse_address(true));
    im.acceptor.bind(endpoint, ec);
    if (ec) throw std::runtime_error("http: bind failed on " + im.options.host + ":" + std::to_string(im.options.port) + ": " + ec.message());
    im.acceptor.listen(asio::socket_base::max_listen_connections);
    im.port = im.acceptor.local_endpoint().port();
    im.running = true;
    im.work.emplace(im.io.get_executor());
    im.accept();
    for (unsigned i = 0; i < std::max(1u, im.options.io_threads); ++i) im.threads.emplace_back([&im] { im.io.run(); });
}

std::uint16_t Server::bound_port() const { return impl_->port; }

void Server::stop() {
    auto& im = *impl_;
    if (!im.running.exchange(false)) {
        if (im.threads.empty()) return;
    }
    asio::post(im.io, [&im] {
        beast::error_code ec;
        im.acceptor.close(ec);
    });
    im.work.reset();
    im.io.stop();
    for (auto& t : im.threads)
        if (t.joinable()) t.join();
    im.threads.clear();
    if (im.handlers) im.handlers->stop();
}

} // namespace di::http
