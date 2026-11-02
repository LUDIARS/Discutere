#include "api/worker.hpp"

#include <cmath>
#include <limits>
#include <mutex>

#include "middleware/auth.hpp"

namespace di::api {

namespace {

std::mutex g_mutex;
std::shared_ptr<persona_engine::worker_pool::WorkerPool> g_pool;

std::shared_ptr<persona_engine::worker_pool::WorkerPool> current_pool() {
    std::lock_guard lock(g_mutex);
    return g_pool;
}

std::optional<double> num(const Json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return std::nullopt;
    const double v = it->get<double>();
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

const Json* field(const Json& body, const char* key) {
    if (!body.is_object()) return nullptr;
    auto it = body.find(key);
    return it == body.end() ? nullptr : &*it;
}

/** 整数として表せる数値 (lo..hi)。それ以外は nullopt。 */
std::optional<std::int64_t> as_int_in(const Json* v, double lo, double hi) {
    if (!v || !v->is_number()) return std::nullopt;
    const double d = v->get<double>();
    if (!std::isfinite(d) || d != std::floor(d) || d < lo || d > hi) return std::nullopt;
    return static_cast<std::int64_t>(d);
}

} // namespace

void set_worker_pool(std::shared_ptr<persona_engine::worker_pool::WorkerPool> pool) {
    std::lock_guard lock(g_mutex);
    g_pool = std::move(pool);
}

JsonLimits worker_callback_json_limits() { return JsonLimits{http::kDefaultMaxBody, 20'000, 32}; }

std::optional<persona_engine::TokenUsage> pick_usage(const Json& raw) {
    if (!raw.is_object()) return std::nullopt;
    persona_engine::TokenUsage u;
    u.input_tokens = num(raw, "input_tokens");
    u.output_tokens = num(raw, "output_tokens");
    u.cache_read_input_tokens = num(raw, "cache_read_input_tokens");
    u.cache_creation_input_tokens = num(raw, "cache_creation_input_tokens");
    u.cost_usd = num(raw, "cost_usd");
    // 実モデル id (transcript の message.model 由来)。cost 補完がモデル単価を引くのに使う。
    if (auto it = raw.find("model"); it != raw.end() && it->is_string() && !it->get<std::string>().empty())
        u.model = it->get<std::string>();
    const bool has_any = u.input_tokens || u.output_tokens || u.cache_read_input_tokens || u.cache_creation_input_tokens;
    if (!has_any) return std::nullopt;
    return u;
}

http::Router worker_routes() {
    http::Router router;

    // 多層防御 (REVIEW_VULNERABILITY H-1): サーバを loopback bind しても Cloudflare Tunnel 等は 127.0.0.1 から
    // 中継しうる。worker callback は常駐ワーカー (同一ホスト) 専用なので、非 loopback 呼び出しは経路ごと 403。
    router.use("/worker/*", [](http::Request& req, const http::Next& next) {
        if (!middleware::is_loopback_request(req)) return http::Response::json(Json{{"error", "loopback only"}}, 403);
        return next();
    });

    router.post("/worker/register", [](http::Request& req) {
        auto pool = current_pool();
        if (!pool) return http::Response::json(Json{{"error", "worker pool not initialized"}}, 503);
        const auto body = req.json(worker_callback_json_limits());
        const Json* worker_id = body ? field(*body, "workerId") : nullptr;
        const Json* port = body ? field(*body, "lictorPort") : nullptr;
        // TS は number なら何でも受けていた。C++ はポートとして使える整数 (0..65535) だけを受ける。
        const auto port_value = as_int_in(port, 0, 65535);
        if (!worker_id || !worker_id->is_string() || !port_value)
            return http::Response::json(Json{{"error", "workerId (string) and lictorPort (number) required"}}, 400);
        // lictorPid > 0 の整数だけ (kill の対象になるので範囲外は捨てる)。
        const auto pid = as_int_in(field(*body, "lictorPid"), 1, static_cast<double>(std::numeric_limits<std::int32_t>::max()));
        const bool ok = pool->register_port(worker_id->get<std::string>(), *port_value, pid);
        return http::Response::json(Json{{"ok", ok}});
    });

    router.post("/worker/utterance", [](http::Request& req) {
        auto pool = current_pool();
        if (!pool) return http::Response::json(Json{{"error", "worker pool not initialized"}}, 503);
        const auto body = req.json(worker_callback_json_limits());
        const Json* req_id = body ? field(*body, "reqId") : nullptr;
        if (!req_id || !req_id->is_string()) return http::Response::json(Json{{"error", "reqId (string) required"}}, 400);
        const Json* worker_id = field(*body, "workerId");
        const Json* text = field(*body, "text");
        const Json* usage = field(*body, "usage");
        pool->on_utterance(req_id->get<std::string>(), worker_id && worker_id->is_string() ? worker_id->get<std::string>() : "",
                           text && text->is_string() ? text->get<std::string>() : "", usage ? pick_usage(*usage) : std::nullopt);
        return http::Response::json(Json{{"ok", true}});
    });

    return router;
}

} // namespace di::api
