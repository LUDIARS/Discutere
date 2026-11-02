#include "api/dashboard_routes.hpp"

#include "api/web_page.hpp"
#include "middleware/auth.hpp"

namespace di::api {

http::Router dashboard_routes() {
    http::Router router;
    router.get("/admin/dashboard", [](http::Request& request) {
        if (auto guard = middleware::require_admin(request)) return *guard;
        return http::Response::html(std::string(web_page("api/dashboard.html")));
    });
    return router;
}

} // namespace di::api
