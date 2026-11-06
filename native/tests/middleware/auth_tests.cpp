// middleware/auth (userContext / requireRole / isLoopbackRequest) と admin guard の境界。
// golden: native/tests/golden/w1-F-golden.gen.ts が Hono で記録した応答 (fixtures/w1f/auth.json)。
#include "check.hpp"
#include "w1f_support.hpp"

#include "api/dashboard_routes.hpp"
#include "api/errors_routes.hpp"
#include "middleware/auth.hpp"

using namespace di;
using namespace di::test::w1f;

namespace {

http::Router app() {
    http::Router router;
    router.use("*", middleware::user_context());
    router.get("/who", [](http::Request& req) {
        return http::Response::json(Json{{"userId", middleware::get_user_id(req)},
                                         {"userRole", middleware::get_user_role(req)},
                                         {"loopback", middleware::is_loopback_request(req)}});
    });
    router.use("/admin-only", middleware::require_role("admin"));
    router.get("/admin-only", [](http::Request&) { return http::Response::json(Json{{"ok", true}}); });
    router.mount("/api", api::dashboard_routes());
    router.mount("/api", api::errors_routes());
    return router;
}

} // namespace

TEST_CASE("userContext / requireRole / admin guard match Hono for every address x header set") {
    const Json fx = load_fixture("auth.json");
    const auto router = app();
    int n = 0;
    for (const auto& c : fx["cases"]) {
        for (const char* path : {"/who", "/admin-only", "/api/admin/dashboard", "/api/admin/errors?limit=1"}) {
            auto req = make_request("GET", path, c["headers"], c["remoteAddress"].get<std::string>());
            const auto res = router.handle(req);
            const Json& expected = c[path];
            const std::string label = json_stringify(c["remoteAddress"]) + " " + json_stringify(c["headers"]) + " " + path;
            if (res.status != expected["status"].get<int>()) throw test::Failure(label + " status " + std::to_string(res.status));
            if (!expected["body"].is_null() && json_stringify(body_json(res)) != json_stringify(expected["body"]))
                throw test::Failure(label + " body " + res.body);
            ++n;
        }
    }
    CHECK(n > 300);
}

TEST_CASE("accessors fall back to headers without userContext") {
    const Json fx = load_fixture("auth.json");
    for (const auto& c : fx["accessors"]) {
        auto req = make_request("GET", "/acc", c["headers"], "127.0.0.1");
        Json body = Json::object();
        body["userId"] = middleware::get_user_id(req);
        body["userRole"] = middleware::get_user_role(req);
        body["userName"] = middleware::get_user_name(req);
        auto email = middleware::get_user_email(req);
        body["userEmail"] = email ? Json(*email) : Json(nullptr);
        CHECK_EQ(json_stringify(body), json_stringify(c["body"]));
    }
}

TEST_CASE("dashboard / errors guards without userContext (header only)") {
    const Json fx = load_fixture("auth.json");
    const auto dashboard = api::dashboard_routes();
    const auto errors = api::errors_routes();
    for (const auto& c : fx["routesOnly"]) {
        auto d = make_request("GET", "/admin/dashboard", c["headers"], "127.0.0.1");
        const auto dr = dashboard.handle(d);
        CHECK_EQ(dr.status, c["dashboard"]["status"].get<int>());
        if (!c["dashboard"]["json"].is_null()) CHECK_EQ(json_stringify(body_json(dr)), json_stringify(c["dashboard"]["json"]));
        auto e = make_request("POST", "/admin/errors/clear", c["headers"], "127.0.0.1");
        const auto er = errors.handle(e);
        CHECK_EQ(er.status, c["clear"]["status"].get<int>());
        CHECK_EQ(json_stringify(body_json(er)), json_stringify(c["clear"]["json"]));
    }
}

TEST_CASE("loopback boundary: only exact loopback forms count") {
    const auto router = app();
    auto who = [&](const std::string& addr, const Json& headers) {
        auto req = make_request("GET", "/who", headers, addr);
        return body_json(router.handle(req));
    };
    for (const char* addr : {"127.0.0.1", "::1", "::ffff:127.0.0.1"}) CHECK_EQ(who(addr, Json::object())["userRole"], "admin");
    for (const char* addr : {"192.168.1.10", "10.0.0.5", "::ffff:192.168.1.10", "127.0.0.2", "0:0:0:0:0:0:0:1", "", "localhost",
                             "127.0.0.1 ", "::ffff:127.0.0.1:80", "fe80::1"}) {
        const Json w = who(addr, Json::object());
        CHECK_EQ(w["userId"], "anonymous");
        CHECK_EQ(w["userRole"], "general");
    }
    // ヘッダ偽装: LAN から role だけ送っても admin にならない (userId 無しは anonymous)。
    CHECK_EQ(who("192.168.1.10", Json{{"X-User-Role", "admin"}})["userRole"], "general");
    // 明示ヘッダは loopback より優先 (loopback から一般ユーザとして名乗ると admin にならない)。
    CHECK_EQ(who("127.0.0.1", Json{{"X-User-Id", "u"}})["userRole"], "general");
    // 大文字小文字の違う role は admin ではない。
    auto req = make_request("GET", "/admin-only", Json{{"X-User-Id", "u"}, {"X-User-Role", "ADMIN"}}, "127.0.0.1");
    CHECK_EQ(router.handle(req).status, 403);
}

TEST_CASE("require_role reads only the context (headers alone do not pass)") {
    http::Router router;
    router.use("*", middleware::require_role("admin"));
    router.get("/x", [](http::Request&) { return http::Response::json(Json{{"ok", true}}); });
    auto req = make_request("GET", "/x", Json{{"X-User-Id", "u"}, {"X-User-Role", "admin"}}, "127.0.0.1");
    const auto res = router.handle(req);
    CHECK_EQ(res.status, 403);
    CHECK_EQ(res.body, R"({"error":"Role 'admin' required"})");
}

DI_TEST_MAIN()
