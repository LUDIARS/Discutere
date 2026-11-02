#include "api/errors_routes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/strings.hpp"
#include "middleware/auth.hpp"
#include "observability/error_buffer.hpp"

namespace di::api {

http::Router errors_routes() {
    http::Router router;
    router.get("/admin/errors", [](http::Request& request) {
        if (auto guard = middleware::require_admin(request)) return *guard;
        // Math.max(1, Math.min(200, Number(c.req.query("limit") ?? 100)))。NaN は NaN のまま (全件)。
        double limit = 100;
        if (auto q = request.query("limit")) {
            auto n = str::js_number(*q);
            limit = n ? *n : std::numeric_limits<double>::quiet_NaN();
        }
        if (!std::isnan(limit)) limit = std::max(1.0, std::min(200.0, limit));
        Json errors = Json::array();
        for (const auto& e : observability::recent_errors(limit)) errors.push_back(observability::to_json(e));
        return http::Response::json(Json{{"errors", errors}});
    });
    router.post("/admin/errors/clear", [](http::Request& request) {
        if (auto guard = middleware::require_admin(request)) return *guard;
        observability::clear_errors();
        return http::Response::json(Json{{"ok", true}});
    });
    return router;
}

} // namespace di::api
