#include "api/queue_routes.hpp"

#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "middleware/auth.hpp"
#include "queue/snapshot.hpp"

namespace di::api {

http::Router queue_routes() {
    http::Router routes;
    routes.get("/admin/queue", [](http::Request& req) {
        if (auto guard = middleware::require_admin(req)) return *guard;
        const auto& config = config::get_config();
        core::CoreOptions co;
        co.sqlitePath = core::resolve_active_kg_path(config);
        auto core = core::create_core(co);
        std::shared_ptr<sql::Database> pe_db;
        try {
            sql::OpenOptions oo;
            oo.readonly = true;
            oo.fileMustExist = true;
            oo.busy_timeout_ms = 2000;
            pe_db = sql::Database::open(config.personaEngine.dbPath, oo);
        } catch (...) {
            pe_db.reset();
        }
        struct Closer {
            core::Core& c;
            std::shared_ptr<sql::Database>& pe;
            ~Closer() {
                try {
                    if (pe) pe->close();
                } catch (...) {
                }
                try {
                    c.close();
                } catch (...) {
                }
            }
        } closer{*core, pe_db};
        const auto snapshot = queue::build_queue_snapshot(core->raw(), pe_db.get(), config.workspace);
        return http::Response::json(queue::to_json(snapshot));
    });
    return routes;
}

} // namespace di::api
