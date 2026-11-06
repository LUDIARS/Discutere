#include "infra/http_client.hpp"

#include <algorithm>
#include <optional>
#include <semaphore>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>

#include "base/strings.hpp"
#include "base/url.hpp"
#include "infra/http_attempt.hpp"

namespace di::http {
namespace asio = boost::asio;

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

std::optional<std::string> resolve_location(const url::Url& base, const std::string& location) {
    if (location.find("://") != std::string::npos) return location;
    if (str::starts_with(location, "//")) return base.scheme + ":" + location;
    if (str::starts_with(location, "/")) return base.origin() + location;
    const auto slash = base.path.rfind('/');
    return base.origin() + base.path.substr(0, slash + 1) + location;
}

} // namespace

namespace {
FetchErrorKind classify(const std::string& e) {
    if (e.empty()) return FetchErrorKind::none;
    if (str::starts_with(e, "timeout")) return FetchErrorKind::timeout;
    if (str::starts_with(e, "blocked")) return FetchErrorKind::blocked;
    if (str::starts_with(e, "busy")) return FetchErrorKind::busy;
    if (str::starts_with(e, "dns")) return FetchErrorKind::dns;
    if (str::starts_with(e, "invalid url")) return FetchErrorKind::invalid_url;
    if (str::starts_with(e, "too many redirects")) return FetchErrorKind::too_many_redirects;
    if (str::contains(e, "body limit") || str::contains(e, "response too large") || str::contains(e, "header limit"))
        return FetchErrorKind::too_large;
    return FetchErrorKind::network;
}
ClientResponse fetch_impl(const ClientRequest& request);
} // namespace

ClientResponse fetch(const ClientRequest& request) {
    ClientResponse out = fetch_impl(request);
    if (str::contains(out.error, "body limit") || str::contains(out.error, "header limit")) out.error = "response too large";
    out.error_kind = classify(out.error);
    return out;
}

namespace {
ClientResponse fetch_impl(const ClientRequest& request) {
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
        detail::Attempt a;
        asio::io_context io;
        detail::AttemptArgs args{*u, method, body, drop_credentials, std::min(now + request.connect_timeout, total_deadline)};
        if (u->scheme == "https")
            detail::start_tls_attempt(io, request, std::move(args), a);
        else
            detail::start_plain_attempt(io, request, std::move(args), a);
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
        out.reason = std::move(a.reason);
        out.headers = std::move(a.headers);
        out.body = std::move(a.body);
        out.final_url = current;
        return out;
    }
    out.error = "too many redirects";
    return out;
}

} // namespace

} // namespace di::http
