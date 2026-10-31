#include "infra/http.hpp"

#include "base/strings.hpp"
#include "base/url.hpp"

namespace di::http {

std::optional<std::string> Headers::get(std::string_view name) const {
    for (const auto& [k, v] : items_)
        if (str::iequals_ascii(k, name)) return v;
    return std::nullopt;
}

bool Headers::has(std::string_view name) const { return get(name).has_value(); }

void Headers::set(std::string name, std::string value) {
    remove(name);
    items_.emplace_back(std::move(name), std::move(value));
}

void Headers::add(std::string name, std::string value) { items_.emplace_back(std::move(name), std::move(value)); }

void Headers::remove(std::string_view name) {
    std::erase_if(items_, [&](const auto& kv) { return str::iequals_ascii(kv.first, name); });
}

std::optional<std::string> Request::query(std::string_view name) const {
    for (const auto& [k, v] : url::parse_query(query_string))
        if (k == name) return v;
    return std::nullopt;
}

std::vector<std::string> Request::queries(std::string_view name) const {
    std::vector<std::string> out;
    for (const auto& [k, v] : url::parse_query(query_string))
        if (k == name) out.push_back(v);
    return out;
}

std::optional<std::string> Request::header(std::string_view name) const { return headers.get(name); }

std::string Request::param(std::string_view name) const {
    auto it = params.find(std::string(name));
    if (it == params.end()) return {};
    auto decoded = url::decode_component(it->second);
    return decoded ? *decoded : it->second;
}

std::optional<Json> Request::json(const JsonLimits& limits) const { return parse_json(body, limits); }

bool Request::from_loopback() const {
    return remote_address == "127.0.0.1" || remote_address == "::1" || remote_address == "::ffff:127.0.0.1";
}

std::string Request::var(std::string_view name) const {
    auto it = vars.find(std::string(name));
    return it == vars.end() ? std::string() : it->second;
}

Response Response::json(const Json& value, int status) {
    Response r;
    r.status = status;
    r.headers.set("Content-Type", "application/json");
    r.body = json_stringify(value);
    return r;
}

Response Response::html(std::string text, int status) {
    Response r;
    r.status = status;
    r.headers.set("Content-Type", "text/html; charset=UTF-8");
    r.body = std::move(text);
    return r;
}

Response Response::text(std::string text, int status) {
    Response r;
    r.status = status;
    r.headers.set("Content-Type", "text/plain; charset=UTF-8");
    r.body = std::move(text);
    return r;
}

Response Response::redirect(std::string location, int status) {
    Response r;
    r.status = status;
    r.headers.set("Location", std::move(location));
    return r;
}

Response Response::bytes(std::string data, std::string content_type, int status) {
    Response r;
    r.status = status;
    r.headers.set("Content-Type", std::move(content_type));
    r.body = std::move(data);
    return r;
}

Response Response::empty(int status) {
    Response r;
    r.status = status;
    return r;
}

bool Router::match(std::string_view pattern, std::string_view path, std::map<std::string, std::string>* params) {
    if (pattern == "*" || pattern == "/*") return true;
    if (!pattern.empty() && pattern.back() == '*') {
        // "/api/*" は "/api" と "/api/..." に、"/abc*" は "/abc..." に一致する (Hono と同じ)。
        std::string_view prefix = pattern.substr(0, pattern.size() - 1);
        if (str::ends_with(prefix, "/")) {
            const std::string_view bare = prefix.substr(0, prefix.size() - 1);
            if (path == bare) return true;
        }
        if (prefix.find(':') == std::string_view::npos) return str::starts_with(path, prefix);
        // パラメータを含む前方一致: セグメント単位で前半を照合する。
        const auto pat_parts = str::split(prefix, "/");
        const auto path_parts = str::split(path, "/");
        if (path_parts.size() < pat_parts.size()) return false;
        std::map<std::string, std::string> found;
        for (std::size_t i = 0; i < pat_parts.size(); ++i) {
            const auto& pp = pat_parts[i];
            if (i + 1 == pat_parts.size() && pp.empty()) break;
            if (!pp.empty() && pp[0] == ':') {
                if (path_parts[i].empty()) return false;
                found[pp.substr(1)] = path_parts[i];
            } else if (pp != path_parts[i]) {
                return false;
            }
        }
        if (params)
            for (auto& [k, v] : found) (*params)[k] = v;
        return true;
    }
    const auto pat_parts = str::split(pattern, "/");
    const auto path_parts = str::split(path, "/");
    if (pat_parts.size() != path_parts.size()) return false;
    std::map<std::string, std::string> found;
    for (std::size_t i = 0; i < pat_parts.size(); ++i) {
        const auto& pp = pat_parts[i];
        if (!pp.empty() && pp[0] == ':') {
            if (path_parts[i].empty()) return false;
            found[pp.substr(1)] = path_parts[i];
        } else if (pp != path_parts[i]) {
            return false;
        }
    }
    if (params)
        for (auto& [k, v] : found) (*params)[k] = v;
    return true;
}

void Router::on(std::string method, std::string pattern, Handler handler, RouteOptions options) {
    Entry e{Entry::Kind::route, str::to_upper(method), std::move(pattern), std::move(handler), {}, options.max_body};
    entries_.push_back(std::move(e));
}

void Router::get(std::string p, Handler h, RouteOptions o) { on("GET", std::move(p), std::move(h), o); }
void Router::post(std::string p, Handler h, RouteOptions o) { on("POST", std::move(p), std::move(h), o); }
void Router::put(std::string p, Handler h, RouteOptions o) { on("PUT", std::move(p), std::move(h), o); }
void Router::del(std::string p, Handler h, RouteOptions o) { on("DELETE", std::move(p), std::move(h), o); }
void Router::patch(std::string p, Handler h, RouteOptions o) { on("PATCH", std::move(p), std::move(h), o); }

void Router::use(std::string pattern, Middleware middleware) {
    Entry e{Entry::Kind::middleware, "*", std::move(pattern), {}, std::move(middleware), 0};
    entries_.push_back(std::move(e));
}

void Router::body_limit(std::string pattern, std::size_t max_body) {
    Entry e{Entry::Kind::limit, "*", std::move(pattern), {}, {}, max_body};
    entries_.push_back(std::move(e));
}

namespace {
std::string merge_path(std::string_view prefix, std::string_view pattern) {
    std::string p(prefix);
    while (!p.empty() && p.back() == '/') p.pop_back();
    if (pattern == "*") return p.empty() ? "*" : p + "/*";
    if (pattern == "/" || pattern.empty()) return p.empty() ? "/" : p;
    if (pattern.front() != '/') return p + "/" + std::string(pattern);
    return p + std::string(pattern);
}
} // namespace

void Router::mount(std::string_view prefix, const Router& sub) {
    for (const auto& e : sub.entries_) {
        Entry copy = e;
        copy.pattern = merge_path(prefix, e.pattern);
        entries_.push_back(std::move(copy));
    }
}

std::size_t Router::max_body_for(std::string_view method, std::string_view path) const {
    const std::string m = str::to_upper(method);
    std::optional<std::size_t> limit_mw;
    for (const auto& e : entries_) {
        if (e.kind == Entry::Kind::limit && match(e.pattern, path, nullptr)) {
            limit_mw = limit_mw ? std::min(*limit_mw, e.max_body) : e.max_body;
        }
        if (e.kind == Entry::Kind::route && (e.method == "*" || e.method == m || (m == "HEAD" && e.method == "GET")) &&
            match(e.pattern, path, nullptr)) {
            return limit_mw ? std::min(*limit_mw, e.max_body) : e.max_body;
        }
    }
    return limit_mw ? *limit_mw : kDefaultMaxBody;
}

Response Router::handle(Request& request) const {
    const std::string method = str::to_upper(request.method);
    const bool head = method == "HEAD";
    std::vector<const Entry*> chain;
    for (const auto& e : entries_) {
        if (e.kind == Entry::Kind::limit) continue;
        if (e.kind == Entry::Kind::route) {
            const bool method_ok = e.method == "*" || e.method == method || (head && e.method == "GET");
            if (!method_ok) continue;
        }
        if (match(e.pattern, request.path, nullptr)) chain.push_back(&e);
    }
    std::function<Response(std::size_t)> step = [&](std::size_t i) -> Response {
        for (; i < chain.size(); ++i) {
            const Entry* e = chain[i];
            if (e->kind == Entry::Kind::middleware) {
                const std::size_t next_index = i + 1;
                Next next = [&step, next_index]() { return step(next_index); };
                return e->middleware(request, next);
            }
            request.params.clear();
            Router::match(e->pattern, request.path, &request.params);
            return e->handler(request);
        }
        return Response::text("404 Not Found", 404);
    };
    Response response = step(0);
    if (head) response.body.clear();
    return response;
}

Middleware cors(std::string origin, bool credentials, std::vector<std::string> allow_methods, std::vector<std::string> allow_headers) {
    return [origin = std::move(origin), credentials, methods = str::join(allow_methods, ","),
            headers = str::join(allow_headers, ",")](Request& req, const Next& next) -> Response {
        auto decorate = [&](Response& r) {
            r.headers.set("Access-Control-Allow-Origin", origin);
            if (credentials) r.headers.set("Access-Control-Allow-Credentials", "true");
            if (origin != "*") {
                auto vary = r.headers.get("Vary");
                r.headers.set("Vary", vary ? *vary + ", Origin" : std::string("Origin"));
            }
        };
        if (str::to_upper(req.method) == "OPTIONS") {
            Response r = Response::empty(204);
            decorate(r);
            if (!methods.empty()) r.headers.set("Access-Control-Allow-Methods", methods);
            if (!headers.empty()) {
                r.headers.set("Access-Control-Allow-Headers", headers);
                r.headers.set("Vary", "Access-Control-Request-Headers");
                if (origin != "*") r.headers.set("Vary", "Access-Control-Request-Headers, Origin");
            }
            return r;
        }
        Response r = next();
        decorate(r);
        return r;
    };
}

} // namespace di::http
