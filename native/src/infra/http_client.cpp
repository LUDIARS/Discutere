#include "infra/http_client.hpp"

#include <optional>
#include <semaphore>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <lorica/input_budget.hpp>

#include "base/strings.hpp"
#include "base/url.hpp"
#include "infra/tls.hpp"

namespace di::http {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = asio::ip::tcp;

bool is_public_address(const std::string& ip) {
    boost::system::error_code ec;
    auto addr = asio::ip::make_address(ip, ec);
    if (ec) return false;
    if (addr.is_v6()) {
        auto v6 = addr.to_v6();
        if (v6.is_v4_mapped()) {
            addr = asio::ip::make_address_v4(asio::ip::v4_mapped, v6);
        } else {
            if (v6.is_loopback() || v6.is_unspecified() || v6.is_link_local() || v6.is_multicast() || v6.is_site_local())
                return false;
            const auto b = v6.to_bytes();
            if ((b[0] & 0xFE) == 0xFC) return false;  // fc00::/7 (ULA)
            if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return false;  // 2001:db8::/32
            if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) return false;  // 64:ff9b::/96 (NAT64)
            return true;
        }
    }
    const auto b = addr.to_v4().to_bytes();
    if (b[0] == 0 || b[0] == 10 || b[0] == 127) return false;
    if (b[0] == 169 && b[1] == 254) return false;
    if (b[0] == 172 && (b[1] & 0xF0) == 16) return false;
    if (b[0] == 192 && b[1] == 168) return false;
    if (b[0] == 100 && (b[1] & 0xC0) == 64) return false;  // 100.64/10
    if (b[0] == 192 && b[1] == 0 && b[2] == 0) return false;
    if (b[0] == 198 && (b[1] & 0xFE) == 18) return false;  // 198.18/15
    if (b[0] >= 224) return false;                          // multicast / reserved / broadcast
    return true;
}

namespace {

std::counting_semaphore<32>& outbound_slots() {
    static std::counting_semaphore<32> slots(32);
    return slots;
}

struct Attempt {
    bool done = false;
    std::string error;
    int status = 0;
    Headers headers;
    std::string body;
};

template <class Stream>
asio::awaitable<void> exchange(Stream& stream, const url::Url& u, const ClientRequest& req, const std::string& method,
                               const std::string& body, bool drop_credentials, Attempt& out) {
    bhttp::request<bhttp::string_body> r;
    r.version(11);
    r.method_string(method);
    r.target(u.target);
    r.set(bhttp::field::host, u.host_for_header() + (u.explicit_port ? ":" + std::to_string(u.port) : ""));
    bool has_ua = false, has_accept = false;
    for (const auto& [k, v] : req.headers.items()) {
        if (k.find_first_of("\r\n") != std::string::npos || v.find_first_of("\r\n") != std::string::npos) continue;
        if (str::iequals_ascii(k, "host") || str::iequals_ascii(k, "content-length") || str::iequals_ascii(k, "accept-encoding"))
            continue;
        if (drop_credentials && (str::iequals_ascii(k, "authorization") || str::iequals_ascii(k, "cookie"))) continue;
        if (str::iequals_ascii(k, "user-agent")) has_ua = true;
        if (str::iequals_ascii(k, "accept")) has_accept = true;
        r.insert(k, v);
    }
    if (!has_ua) r.set(bhttp::field::user_agent, "discutere-native/0.1");
    if (!has_accept) r.set(bhttp::field::accept, "*/*");
    r.set(bhttp::field::connection, "close");
    if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
        r.body() = body;
        r.prepare_payload();
    }
    co_await bhttp::async_write(stream, r, asio::use_awaitable);
    beast::flat_buffer buffer;
    bhttp::response_parser<bhttp::string_body> parser;
    parser.header_limit(64 * 1024);
    parser.body_limit(req.max_response_bytes);
    co_await bhttp::async_read(stream, buffer, parser, asio::use_awaitable);
    auto res = parser.release();
    lorica::input_budget budget({req.max_response_bytes, 1, 1, 1});
    if (!budget.consume(res.body().size(), 1, 1)) throw std::runtime_error("response too large");
    out.status = static_cast<int>(res.result_int());
    for (const auto& f : res) out.headers.add(std::string(f.name_string()), std::string(f.value()));
    out.body = std::move(res.body());
    out.done = true;
}

asio::awaitable<void> attempt(const url::Url& u, const ClientRequest& req, const std::string& method, const std::string& body,
                              bool drop_credentials, std::chrono::steady_clock::time_point connect_deadline, Attempt& out) {
    auto ex = co_await asio::this_coro::executor;
    tcp::resolver resolver(ex);
    auto results = co_await resolver.async_resolve(u.host, std::to_string(u.port), asio::use_awaitable);
    std::vector<tcp::endpoint> endpoints;
    for (const auto& r : results) {
        const auto ep = r.endpoint();
        if (!req.allow_private_network && !is_public_address(ep.address().to_string())) {
            out.error = "blocked: " + u.host + " resolves to a non-public address";
            co_return;
        }
        endpoints.push_back(ep);
    }
    if (endpoints.empty()) {
        out.error = "dns: no address for " + u.host;
        co_return;
    }
    beast::tcp_stream tcp_stream(ex);
    tcp_stream.expires_at(connect_deadline);
    // 検査した IP にだけ接続する (解決し直さない = 接続先 IP の固定)。
    co_await tcp_stream.async_connect(endpoints, asio::use_awaitable);
    tcp_stream.expires_never();
    if (u.scheme == "https") {
        beast::ssl_stream<beast::tcp_stream> tls(std::move(tcp_stream), infra::tls_client_context());
        boost::system::error_code ec;
        asio::ip::make_address(u.host, ec);
        if (ec && !SSL_set_tlsext_host_name(tls.native_handle(), u.host.c_str()))
            throw std::runtime_error("tls: SNI failed");
        tls.set_verify_callback(ssl::host_name_verification(u.host));
        co_await tls.async_handshake(ssl::stream_base::client, asio::use_awaitable);
        co_await exchange(tls, u, req, method, body, drop_credentials, out);
        boost::system::error_code ignore;
        tls.next_layer().socket().close(ignore);
    } else {
        co_await exchange(tcp_stream, u, req, method, body, drop_credentials, out);
        boost::system::error_code ignore;
        tcp_stream.socket().close(ignore);
    }
}

std::optional<std::string> resolve_location(const url::Url& base, const std::string& location) {
    if (location.find("://") != std::string::npos) return location;
    if (str::starts_with(location, "//")) return base.scheme + ":" + location;
    if (str::starts_with(location, "/")) return base.origin() + location;
    const auto slash = base.path.rfind('/');
    return base.origin() + base.path.substr(0, slash + 1) + location;
}

} // namespace

ClientResponse fetch(const ClientRequest& request) {
    ClientResponse out;
    const auto start = std::chrono::steady_clock::now();
    const auto total_deadline = start + request.timeout;
    if (!outbound_slots().try_acquire_for(request.connect_timeout)) {
        out.error = "busy: too many concurrent outbound requests";
        return out;
    }
    struct Release {
        ~Release() { outbound_slots().release(); }
    } release;

    std::string current = request.url;
    std::string method = str::to_upper(request.method);
    std::string body = request.body;
    std::optional<std::string> first_origin;
    for (int hop = 0; hop <= std::max(0, request.max_redirects); ++hop) {
        auto u = url::parse(current);
        if (!u || (u->scheme != "http" && u->scheme != "https")) {
            out.error = "invalid url";
            return out;
        }
        if (!first_origin) first_origin = u->origin();
        const bool drop_credentials = u->origin() != *first_origin;
        const auto now = std::chrono::steady_clock::now();
        if (now >= total_deadline) {
            out.error = "timeout";
            return out;
        }
        Attempt a;
        asio::io_context io;
        asio::co_spawn(io, attempt(*u, request, method, body, drop_credentials, std::min(now + request.connect_timeout, total_deadline), a),
                       [&a](std::exception_ptr e) {
                           if (!e) return;
                           try {
                               std::rethrow_exception(e);
                           } catch (const std::exception& ex) {
                               if (a.error.empty()) a.error = ex.what();
                           }
                       });
        io.run_until(total_deadline);
        if (!io.stopped()) {
            io.stop();
            out.error = "timeout";
            return out;
        }
        if (!a.done) {
            out.error = a.error.empty() ? std::string("request failed") : a.error;
            return out;
        }
        const int s = a.status;
        auto location = a.headers.get("Location");
        if ((s == 301 || s == 302 || s == 303 || s == 307 || s == 308) && location && hop < request.max_redirects) {
            auto next = resolve_location(*u, *location);
            if (!next) break;
            if (s == 303 || ((s == 301 || s == 302) && method == "POST")) {
                method = "GET";
                body.clear();
            }
            current = *next;
            continue;
        }
        out.transport_ok = true;
        out.status = s;
        out.headers = std::move(a.headers);
        out.body = std::move(a.body);
        out.final_url = current;
        return out;
    }
    out.error = "too many redirects";
    return out;
}

} // namespace di::http
