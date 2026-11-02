#include "api/top_page_routes.hpp"

#include "api/web_page.hpp"

namespace di::api {

http::Router top_page_routes() {
    http::Router router;
    router.get("/", [](http::Request&) { return http::Response::html(std::string(web_page("api/top-page.html"))); });
    return router;
}

} // namespace di::api
