#include "api/conclusion_publication_routes.hpp"

#include <algorithm>

#include "api/conclusion_publication_editor.hpp"
#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "flow/db/connection.hpp"
#include "integrations/conclusion_publication.hpp"
#include "middleware/auth.hpp"
#include "visualize/conclusion_cache.hpp"

namespace di::api {

namespace {

constexpr std::size_t kMinTokenBytes = 32;
/** PUT 本文の JSON の上限 (4 キーの小さな object しか受けない)。 */
const JsonLimits kPutJsonLimits{http::kDefaultMaxBody, 10'000, 16};

/** Node は header 値を latin1 として読み、Buffer.from(str) で UTF-8 にする。それと同じバイト列を作る。 */
std::string latin1_to_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

/** JS の \s のうち latin1 の範囲に入るもの。 */
bool is_latin1_js_space(unsigned char c) { return c == 0x20 || (c >= 0x09 && c <= 0x0D) || c == 0xA0; }

/** "host[:port]" を読む。ポートは数字のみ。scheme の既定ポートは origin から省く (WHATWG URL と同じ)。 */
std::optional<RequestUrlParts> parse_authority(std::string_view scheme, std::string_view authority) {
    if (authority.empty() || authority.find('@') != std::string_view::npos) return std::nullopt;
    std::string host_part;
    std::string_view port_part;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        host_part = std::string(authority.substr(0, close + 1));
        const auto rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return std::nullopt;
            port_part = rest.substr(1);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            host_part = std::string(authority);
        } else {
            host_part = std::string(authority.substr(0, colon));
            port_part = authority.substr(colon + 1);
        }
    }
    if (host_part.empty()) return std::nullopt;
    for (const char c : host_part)
        if (static_cast<unsigned char>(c) <= 0x20 || c == '/' || c == '\\' || c == '?' || c == '#') return std::nullopt;
    std::string port;
    if (!port_part.empty()) {
        if (port_part.size() > 16) return std::nullopt;
        unsigned long value = 0;
        for (const char c : port_part) {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + static_cast<unsigned long>(c - '0');
        }
        if (value > 65535) return std::nullopt;
        const unsigned long default_port = scheme == "https" ? 443 : 80;
        if (value != default_port) port = std::to_string(value);
    }
    RequestUrlParts out;
    out.hostname = str::to_lower_ascii(host_part);
    out.origin = std::string(scheme) + "://" + out.hostname + (port.empty() ? "" : ":" + port);
    return out;
}

http::Response json_error(const char* error, int status) { return http::Response::json(Json{{"error", error}}, status); }

} // namespace

std::optional<RequestUrlParts> request_url_parts(const http::Request& req) {
    // 絶対形式の要求行 (GET http://host/path) は @hono/node-server がそのまま URL にする。
    const std::string_view target = req.target;
    for (const std::string_view scheme : {std::string_view("http"), std::string_view("https")}) {
        const std::string prefix = std::string(scheme) + "://";
        if (target.size() >= prefix.size() && str::to_lower_ascii(target.substr(0, prefix.size())) == prefix) {
            auto rest = target.substr(prefix.size());
            const auto end = rest.find_first_of("/?#");
            return parse_authority(scheme, rest.substr(0, end));
        }
    }
    const auto host = req.header("host");
    if (!host || host->empty()) return std::nullopt;
    // C++ のサーバは平文 HTTP のみ (TLS は前段で終端する) なので scheme は http。
    return parse_authority("http", *host);
}

std::string bearer_token_of(const std::optional<std::string>& authorization) {
    if (!authorization) return "";
    const std::string_view v = *authorization;
    constexpr std::string_view prefix = "Bearer ";
    if (v.size() <= prefix.size() || v.substr(0, prefix.size()) != prefix) return "";
    const auto token = v.substr(prefix.size());
    for (const char c : token)
        if (is_latin1_js_space(static_cast<unsigned char>(c))) return "";
    return latin1_to_utf8(token);
}

http::Router create_conclusion_publication_routes(DatabaseFn database, TokenFn token) {
    if (!database) database = []() -> sql::Database& { return *flow::db::get_flow_db(); };
    if (!token) token = [] { return config::get_config().glabConclusions.token; };
    http::Router routes;

    routes.use("*", [](http::Request&, const http::Next& next) {
        auto res = next();
        res.headers.set("Cache-Control", "private, no-store");
        return res;
    });

    routes.use("/integrations/glab/conclusions*", [token](http::Request& req, const http::Next& next) {
        const std::string expected = token();
        if (expected.size() < kMinTokenBytes) return json_error("conclusions_unconfigured", 503);
        const std::string actual = bearer_token_of(req.header("authorization"));
        // 長さが違えば比較せず 401 (timingSafeEqual は同じ長さが前提)。中身は定数時間で比べる。
        if (actual.size() != expected.size() || !crypto::constant_time_equal(actual, expected)) return json_error("unauthorized", 401);
        return next();
    });

    routes.get("/integrations/glab/conclusions", [database](http::Request& req) {
        auto& db = database();
        visualize::ensure_conclusion_cache_fresh(db);
        const auto id = req.query("id");
        Json list = Json::array();
        const auto results = integrations::published_conclusions(db, id);
        if (id && results.empty()) return json_error("not_found", 404);
        for (const auto& r : results) list.push_back(integrations::to_json(r));
        return http::Response::json(Json{{"conclusions", list}});
    });

    // 公開設定の変更はローカルの操作者の操作 (GLAB の操作ではない)。Host + Origin の検査で
    // トンネル / プロキシ越しの要求と別サイトのブラウザ送信も拒む。
    routes.use("/admin/conclusion-publication*", [](http::Request& req, const http::Next& next) {
        const auto url = request_url_parts(req);
        if (!middleware::is_loopback_request(req) || !url ||
            (url->hostname != "localhost" && url->hostname != "127.0.0.1" && url->hostname != "[::1]"))
            return json_error("local_operator_required", 403);
        const auto origin = req.header("origin");
        if (origin && !origin->empty() && *origin != url->origin) return json_error("origin_denied", 403);
        return next();
    });

    routes.get("/admin/conclusion-publication/editor", [](http::Request&) { return http::Response::html(std::string(publication_editor_html())); });

    routes.get("/admin/conclusion-publication", [database](http::Request& req) {
        auto& db = database();
        visualize::ensure_conclusion_cache_fresh(db);
        const auto state = integrations::publication_state(db, req.query("id").value_or(""));
        return state ? http::Response::json(integrations::to_json(*state)) : json_error("not_found", 404);
    });

    routes.put("/admin/conclusion-publication", [database](http::Request& req) {
        const auto content_type = req.header("content-type");
        if (!content_type || !str::starts_with(*content_type, "application/json")) return json_error("json_required", 415);
        const auto body = req.json(kPutJsonLimits);
        if (!body || !body->is_object()) return json_error("invalid_input", 400);
        const Json& input = *body;
        for (const auto& [key, value] : input.items()) {
            (void)value;
            if (key != "id" && key != "publishable" && key != "scope" && key != "revision") return json_error("invalid_input", 400);
        }
        auto is_str = [&](const char* k) { return input.contains(k) && input[k].is_string(); };
        if (!is_str("id") || input["id"].get<std::string>().empty() || str::js_length(input["id"].get<std::string>()) > 500 ||
            !(input.contains("publishable") && input["publishable"].is_boolean()) || !is_str("revision") || !is_str("scope"))
            return json_error("invalid_input", 400);
        const std::string scope = input["scope"].get<std::string>();
        if (scope != "private" && scope != "public" && scope != "glab") return json_error("invalid_input", 400);
        const std::string id = input["id"].get<std::string>();
        auto& db = database();
        visualize::ensure_conclusion_cache_fresh(db);
        if (!integrations::set_publication(db, id, input["publishable"].get<bool>(), scope, input["revision"].get<std::string>(), time::now_ms()))
            return json_error("conclusion_changed_or_unavailable", 409);
        const auto state = integrations::publication_state(db, id);
        return http::Response::json(state ? integrations::to_json(*state) : Json(nullptr));
    });

    return routes;
}

} // namespace di::api
