// w3o_admin_routes のテスト (3/3)。準備は w3o_admin_routes_support.hpp。
#include "w3o_admin_routes_support.hpp"

using namespace w3o_admin_routes_support;

// ── kg-migration routes ─────────────────────────────────────────────────────

TEST_CASE("kg-migration: serveToken 未設定は誰でも取得・設定時は ?token= か x-kg-token (定数時間比較)") {
    Json seen = Json::array();
    KgMigrationRouteDeps open;
    open.serveToken = [] { return std::optional<std::string>(); };
    open.exportShared = [&seen](double since) {
        seen.push_back(since);
        kg_sync::KgExportResult r;
        r.ndjson = "{\"manifest\":1}\n";
        r.watermark = 1234.5;
        r.count = 3;
        return r;
    };
    auto app = mounted(kg_migration_routes(open));
    auto r = call(app, "GET", "/api/kg/migrations", "", "", "");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.headers.get("content-type").value_or(""), std::string("application/x-ndjson; charset=utf-8"));
    CHECK_EQ(r.headers.get("x-kg-watermark").value_or(""), std::string("1234.5"));
    CHECK_EQ(r.headers.get("x-kg-count").value_or(""), std::string("3"));
    CHECK_EQ(r.body, std::string("{\"manifest\":1}\n"));
    CHECK_EQ(call(app, "GET", "/api/kg/migrations?since=10").status, 200);
    CHECK_EQ(call(app, "GET", "/api/kg/migrations?since=").status, 200);
    CHECK_EQ(call(app, "GET", "/api/kg/migrations?since=-1").status, 400);
    CHECK_EQ(call(app, "GET", "/api/kg/migrations?since=abc").status, 400);
    CHECK_EQ(call(app, "GET", "/api/kg/migrations?since=1e400").status, 400);
    CHECK_EQ(call(app, "GET", "/api/kg/migrations?since=0x10").status, 200);
    W3O_SAME(seen, (Json::array({0, 10, 0, 16})));

    KgMigrationRouteDeps locked = open;
    locked.serveToken = [] { return std::optional<std::string>("serve-secret-for-tests"); };
    auto guarded = mounted(kg_migration_routes(locked));
    const auto denied = call(guarded, "GET", "/api/kg/migrations");
    CHECK_EQ(denied.status, 403);
    CHECK_EQ(denied.body, std::string("forbidden"));
    CHECK_EQ(call(guarded, "GET", "/api/kg/migrations?token=serve-secret-for-tests").status, 200);
    CHECK_EQ(call(guarded, "GET", "/api/kg/migrations?token=serve-secret-for-test").status, 403);
    CHECK_EQ(call(guarded, "GET", "/api/kg/migrations?token=serve-secret-for-testss").status, 403);
    const std::string oversized_token(1u << 16, 's');
    CHECK_EQ(call(guarded, "GET", "/api/kg/migrations?token=" + oversized_token).status, 403);
    {
        http::Request req;
        req.method = "GET";
        req.target = req.path = "/api/kg/migrations";
        req.headers.set("x-kg-token", "serve-secret-for-tests");
        CHECK_EQ(guarded.handle(req).status, 200);
        // ?token= があれば (空でも) そちらを使う (TS の ?? と同じ)
        req.target = "/api/kg/migrations?token=";
        req.query_string = "token=";
        CHECK_EQ(guarded.handle(req).status, 403);
    }
}

TEST_CASE("kg-migration: 既定の export は active KG の共有テーブルを読む") {
    Env e;
    auto app = mounted(kg_migration_routes({[] { return std::optional<std::string>(); }, {}}));
    const auto r = call(app, "GET", "/api/kg/migrations?since=0");
    CHECK_EQ(r.status, 200);
    CHECK(str::contains(r.body, "manifest"));
    CHECK_EQ(r.headers.get("x-kg-count").value_or(""), std::string("0"));
}

DI_TEST_MAIN()
