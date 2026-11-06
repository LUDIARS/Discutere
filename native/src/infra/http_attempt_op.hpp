#pragma once
// http_attempt_plain.cpp / http_attempt_tls.cpp だけが include する試行の本体。
// コールバックの連鎖で進める (asio::awaitable は使わない: Praesidium の静的解析が Boost の
// コルーチン実装の中で誤検知するため。Odeum の relay と同じ書き方)。
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <lorica/input_budget.hpp>

#include "base/strings.hpp"
#include "infra/http_attempt.hpp"
#include "infra/tls.hpp"

namespace di::http::detail {

template <bool Tls>
class AttemptOp : public std::enable_shared_from_this<AttemptOp<Tls>> {
    using tcp = boost::asio::ip::tcp;
    using error_code = boost::system::error_code;
    using TlsStream = boost::beast::ssl_stream<boost::beast::tcp_stream>;

public:
    AttemptOp(boost::asio::io_context& io, const ClientRequest& req, AttemptArgs args, Attempt& out)
        : req_(req), args_(std::move(args)), out_(out), resolver_(io), tcp_(io) {}

    void start() {
        resolver_.async_resolve(args_.url.host, std::to_string(args_.url.port),
                                [self = this->shared_from_this()](const error_code& ec, const tcp::resolver::results_type& results) {
                                    self->on_resolve(ec, results);
                                });
    }

private:
    void fail(const error_code& ec) {
        if (out_.error.empty()) out_.error = boost::system::system_error(ec).what();
    }
    void fail(std::string message) {
        if (out_.error.empty()) out_.error = std::move(message);
    }

    void on_resolve(const error_code& ec, const tcp::resolver::results_type& results) {
        if (ec) return fail(ec);
        for (const auto& r : results) {
            const auto ep = r.endpoint();
            if (!req_.allow_private_network && !is_public_address(ep.address().to_string()))
                return fail("blocked: " + args_.url.host + " resolves to a non-public address");
            endpoints_.push_back(ep);
        }
        if (endpoints_.empty()) return fail("dns: no address for " + args_.url.host);
        tcp_.expires_at(args_.connect_deadline);
        // 検査した IP にだけ接続する (解決し直さない = 接続先 IP の固定)。
        tcp_.async_connect(endpoints_, [self = this->shared_from_this()](const error_code& e, const tcp::endpoint&) {
            self->on_connect(e);
        });
    }

    void on_connect(const error_code& ec) {
        if (ec) return fail(ec);
        tcp_.expires_never();
        if constexpr (Tls) {
            namespace ssl = boost::asio::ssl;
            tls_.emplace(std::move(tcp_), infra::tls_client_context());
            error_code literal;
            boost::asio::ip::make_address(args_.url.host, literal);
            if (literal && !SSL_set_tlsext_host_name(tls_->native_handle(), args_.url.host.c_str())) return fail("tls: SNI failed");
            tls_->set_verify_callback(ssl::host_name_verification(args_.url.host));
            tls_->async_handshake(ssl::stream_base::client, [self = this->shared_from_this()](const error_code& e) {
                if (e) return self->fail(e);
                self->send();
            });
        } else {
            send();
        }
    }

    auto& stream() {
        if constexpr (Tls) {
            return *tls_;
        } else {
            return tcp_;
        }
    }

    void send() {
        namespace bhttp = boost::beast::http;
        const auto& u = args_.url;
        request_.version(11);
        request_.method_string(args_.method);
        request_.target(u.target);
        request_.set(bhttp::field::host, u.host_for_header() + (u.explicit_port ? ":" + std::to_string(u.port) : ""));
        bool has_ua = false, has_accept = false;
        for (const auto& [k, v] : req_.headers.items()) {
            if (k.find_first_of("\r\n") != std::string::npos || v.find_first_of("\r\n") != std::string::npos) continue;
            if (str::iequals_ascii(k, "host") || str::iequals_ascii(k, "content-length") || str::iequals_ascii(k, "accept-encoding"))
                continue;
            if (args_.drop_credentials && (str::iequals_ascii(k, "authorization") || str::iequals_ascii(k, "cookie"))) continue;
            if (str::iequals_ascii(k, "user-agent")) has_ua = true;
            if (str::iequals_ascii(k, "accept")) has_accept = true;
            request_.insert(k, v);
        }
        if (!has_ua) request_.set(bhttp::field::user_agent, "discutere-native/0.1");
        if (!has_accept) request_.set(bhttp::field::accept, "*/*");
        request_.set(bhttp::field::connection, "close");
        if (!args_.body.empty() || args_.method == "POST" || args_.method == "PUT" || args_.method == "PATCH") {
            request_.body() = args_.body;
            request_.prepare_payload();
        }
        bhttp::async_write(stream(), request_, [self = this->shared_from_this()](const error_code& e, std::size_t) {
            if (e) return self->fail(e);
            self->receive();
        });
    }

    void receive() {
        parser_.emplace();
        parser_->header_limit(64 * 1024);
        parser_->body_limit(req_.max_response_bytes);
        boost::beast::http::async_read(stream(), buffer_, *parser_, [self = this->shared_from_this()](const error_code& e, std::size_t) {
            if (e) return self->fail(e);
            self->finish();
        });
    }

    void finish() {
        auto res = parser_->release();
        lorica::input_budget budget({req_.max_response_bytes, 1, 1, 1});
        if (!budget.consume(res.body().size(), 1, 1)) return fail("response too large");
        out_.status = static_cast<int>(res.result_int());
        out_.reason = std::string(res.reason());
        for (const auto& field : res) out_.headers.add(std::string(field.name_string()), std::string(field.value()));
        out_.body = std::move(res.body());
        out_.done = true;
        error_code ignore;
        boost::beast::get_lowest_layer(stream()).socket().close(ignore);
    }

    const ClientRequest& req_;
    AttemptArgs args_;
    Attempt& out_;
    tcp::resolver resolver_;
    std::vector<tcp::endpoint> endpoints_;
    boost::beast::tcp_stream tcp_;
    std::conditional_t<Tls, std::optional<TlsStream>, std::monostate> tls_;
    boost::beast::http::request<boost::beast::http::string_body> request_;
    boost::beast::flat_buffer buffer_;
    std::optional<boost::beast::http::response_parser<boost::beast::http::string_body>> parser_;
};

} // namespace di::http::detail
