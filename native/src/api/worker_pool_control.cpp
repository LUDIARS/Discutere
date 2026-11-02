#include "api/worker_pool_control.hpp"

#include <mutex>

#include "api/web_page.hpp"

namespace di::api {

namespace {

std::mutex g_mutex;
std::shared_ptr<persona_engine::worker_pool::WorkerPool> g_pool;

std::shared_ptr<persona_engine::worker_pool::WorkerPool> current_pool() {
    std::lock_guard lock(g_mutex);
    return g_pool;
}

Json workers_json(const persona_engine::worker_pool::WorkerPool& pool) {
    Json arr = Json::array();
    for (const auto& w : pool.list_configured()) arr.push_back(persona_engine::worker_pool::to_json(w));
    return arr;
}

} // namespace

void set_worker_pool_control(std::shared_ptr<persona_engine::worker_pool::WorkerPool> pool) {
    std::lock_guard lock(g_mutex);
    g_pool = std::move(pool);
}

http::Router worker_pool_control_routes() {
    http::Router router;

    router.get("/worker-pool/status", [](http::Request&) {
        auto pool = current_pool();
        if (!pool) return http::Response::json(Json{{"enabled", false}, {"workers", Json::array()}});
        return http::Response::json(Json{{"enabled", true}, {"workers", workers_json(*pool)}});
    });

    router.post("/worker-pool/start", [](http::Request&) {
        auto pool = current_pool();
        if (!pool) return http::Response::json(Json{{"error", "worker pool not enabled (backend != worker-pool)"}}, 503);
        pool->start();
        return http::Response::json(Json{{"ok", true}, {"workers", workers_json(*pool)}});
    });

    router.post("/worker-pool/stop", [](http::Request&) {
        auto pool = current_pool();
        if (!pool) return http::Response::json(Json{{"error", "worker pool not enabled"}}, 503);
        pool->stop();
        return http::Response::json(Json{{"ok", true}});
    });

    router.post("/worker-pool/worker/:id/start", [](http::Request& req) {
        auto pool = current_pool();
        if (!pool) return http::Response::json(Json{{"error", "worker pool not enabled"}}, 503);
        const bool ok = pool->start_worker(req.param("id"));
        return http::Response::json(Json{{"ok", ok}}, ok ? 200 : 404);
    });

    router.post("/worker-pool/worker/:id/stop", [](http::Request& req) {
        auto pool = current_pool();
        if (!pool) return http::Response::json(Json{{"error", "worker pool not enabled"}}, 503);
        const bool ok = pool->stop_worker(req.param("id"));
        return http::Response::json(Json{{"ok", ok}}, ok ? 200 : 404);
    });

    router.get("/worker-pool", [](http::Request&) {
        return http::Response::html(std::string(web_page("api/worker-pool-control.html")));
    });

    return router;
}

} // namespace di::api
