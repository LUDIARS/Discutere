#include "api/kg_migration_routes.hpp"

#include <cmath>

#include "base/crypto.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"

namespace di::api {

namespace {

kg_sync::KgExportResult default_export(double since) {
    const auto& config = config::get_config();
    core::CoreOptions co;
    co.sqlitePath = core::resolve_active_kg_path(config);
    auto core = core::create_core(co);
    struct Closer {
        core::Core& c;
        ~Closer() {
            try {
                c.close();
            } catch (...) {
            }
        }
    } closer{*core};
    kg_sync::KgExportOptions opts;
    opts.since = since;
    opts.workspaceId = config.workspace;
    return kg_sync::export_shared_kg(core->raw(), opts);
}

} // namespace

http::Router kg_migration_routes(KgMigrationRouteDeps deps) {
    if (!deps.serveToken) deps.serveToken = [] { return config::get_config().kgSync.serveToken; };
    if (!deps.exportShared) deps.exportShared = default_export;
    http::Router routes;
    routes.get("/kg/migrations", [deps](http::Request& req) {
        const auto required = deps.serveToken();
        if (required && !required->empty()) {
            // c.req.query("token") ?? c.req.header("x-kg-token") (空の ?token= はそのまま比較する)
            auto provided = req.query("token");
            if (!provided) provided = req.header("x-kg-token");
            if (!provided || !crypto::constant_time_equal(*provided, *required)) return http::Response::text("forbidden", 403);
        }

        // Number(sinceRaw) (空文字は 0 と同じ扱い: TS の sinceRaw ? ... : 0)。
        const auto since_raw = req.query("since");
        double since = 0;
        if (since_raw && !since_raw->empty()) {
            const auto n = str::js_number(*since_raw);
            since = n.value_or(std::nan(""));
        }
        if (!std::isfinite(since) || since < 0) return http::Response::text("bad since", 400);

        const auto result = deps.exportShared(since);
        auto r = http::Response::bytes(result.ndjson, "application/x-ndjson; charset=utf-8", 200);
        r.headers.set("x-kg-watermark", js_number_to_string(result.watermark));
        r.headers.set("x-kg-count", std::to_string(result.count));
        return r;
    });
    return routes;
}

} // namespace di::api
