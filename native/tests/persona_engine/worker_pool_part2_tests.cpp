// worker_pool のテスト (2/2)。準備は worker_pool_support.hpp。
#include "worker_pool_support.hpp"

using namespace worker_pool_support;

TEST_CASE("api/worker: loopback 限定 (境界)・503・入力検査・usage") {
    api::set_worker_pool(nullptr);
    auto router = std::make_shared<http::Router>();
    router->mount("/internal", api::worker_routes());
    for (const std::string remote : {"192.168.1.10", "::ffff:192.168.1.10", "10.0.0.1", "", "127.0.0.2", "::2"}) {
        auto req = request("POST", "/internal/worker/register", remote, "{}");
        const auto res = router->handle(req);
        CHECK_EQ(res.status, 403);
        CHECK_EQ(res.body, std::string(R"({"error":"loopback only"})"));
    }
    for (const std::string remote : {"127.0.0.1", "::1", "::ffff:127.0.0.1"}) {
        auto req = request("POST", "/internal/worker/utterance", remote, "{}");
        const auto res = router->handle(req);
        CHECK_EQ(res.status, 503);
        CHECK_EQ(res.body, std::string(R"({"error":"worker pool not initialized"})"));
    }

    const auto dir = w2i::temp_dir("api");
    auto pool = make_pool(dir, std::make_shared<Spawns>());
    pool->start();
    api::set_worker_pool(pool);
    auto post = [&](const std::string& path, const std::string& body) {
        auto req = request("POST", "/internal/worker/" + path, "127.0.0.1", body);
        return router->handle(req);
    };
    const std::string need = R"({"error":"workerId (string) and lictorPort (number) required"})";
    CHECK_EQ(post("register", "not json").body, need);
    CHECK_EQ(post("register", "[1]").body, need);
    CHECK_EQ(post("register", R"({"workerId":"con-opus"})").body, need);
    CHECK_EQ(post("register", R"({"workerId":1,"lictorPort":1})").body, need);
    CHECK_EQ(post("register", R"({"workerId":"con-opus","lictorPort":"1"})").status, 400);
    CHECK_EQ(post("register", R"({"workerId":"con-opus","lictorPort":1.5})").status, 400);
    CHECK_EQ(post("register", R"({"workerId":"con-opus","lictorPort":65536})").status, 400);
    CHECK_EQ(post("register", R"({"workerId":"ghost","lictorPort":65535})").body, std::string(R"({"ok":false})"));
    CHECK_EQ(post("register", R"({"workerId":"con-opus","lictorPort":65535,"lictorPid":-3})").body, std::string(R"({"ok":true})"));
    CHECK(!pool->snapshot()[0].lictorPid);
    CHECK_EQ(post("register", R"({"workerId":"con-opus","lictorPort":4000,"lictorPid":42})").body, std::string(R"({"ok":true})"));
    CHECK_EQ(*pool->snapshot()[0].lictorPid, std::int64_t(42));
    CHECK_EQ(*pool->snapshot()[0].port, std::int64_t(4000));

    CHECK_EQ(post("utterance", "{").body, std::string(R"({"error":"reqId (string) required"})"));
    CHECK_EQ(post("utterance", R"({"reqId":5})").status, 400);
    CHECK_EQ(post("utterance", R"({"reqId":"nope","text":7})").body, std::string(R"({"ok":true})"));

    // 深すぎる JSON は読まない (壊れた入力扱い)
    CHECK_EQ(post("utterance", std::string(100, '[') + std::string(100, ']')).status, 400);

    // usage は既知の数値だけ
    CHECK(!api::pick_usage(Json::array()));
    CHECK(!api::pick_usage(Json{{"cost_usd", 1.0}, {"model", "m"}}));
    const auto u = api::pick_usage(Json{{"input_tokens", 3}, {"output_tokens", "x"}, {"cost_usd", 0.5}, {"model", ""}, {"extra", 1}});
    CHECK(u.has_value());
    CHECK_EQ(*u->input_tokens, 3.0);
    CHECK(!u->output_tokens);
    CHECK_EQ(*u->cost_usd, 0.5);
    CHECK(!u->model);
    CHECK_EQ(*api::pick_usage(Json{{"cache_read_input_tokens", 1}, {"model", "claude"}})->model, std::string("claude"));
    pool->stop();
    api::set_worker_pool(nullptr);
}

TEST_CASE("api/worker: 本文の上限 (1 MiB ちょうど / +1) を実サーバで") {
    api::set_worker_pool(nullptr);
    auto router = std::make_shared<http::Router>();
    router->mount("/internal", api::worker_routes());
    http::ServerOptions o;
    o.port = 0;
    http::Server server(o, router);
    server.start();
    auto send = [&](std::size_t n) {
        http::ClientRequest r;
        r.method = "POST";
        r.url = "http://127.0.0.1:" + std::to_string(server.bound_port()) + "/internal/worker/utterance";
        r.body = std::string(n, ' ');
        r.allow_private_network = true;
        return http::fetch(r).status;
    };
    CHECK_EQ(send(http::kDefaultMaxBody), 503);
    CHECK_EQ(send(http::kDefaultMaxBody + 1), 413);
    server.stop();
}

TEST_CASE("api/worker-pool-control: 状態・起動停止・404・画面") {
    api::set_worker_pool_control(nullptr);
    http::Router router;
    router.mount("/api", api::worker_pool_control_routes());
    auto call = [&](const std::string& method, const std::string& path) {
        auto req = request(method, path, "127.0.0.1");
        return router.handle(req);
    };
    CHECK_EQ(call("GET", "/api/worker-pool/status").body, std::string(R"({"enabled":false,"workers":[]})"));
    CHECK_EQ(call("POST", "/api/worker-pool/start").status, 503);
    CHECK_EQ(call("POST", "/api/worker-pool/start").body, std::string(R"j({"error":"worker pool not enabled (backend != worker-pool)"})j"));
    CHECK_EQ(call("POST", "/api/worker-pool/stop").body, std::string(R"({"error":"worker pool not enabled"})"));
    CHECK_EQ(call("POST", "/api/worker-pool/worker/x/start").status, 503);

    const auto dir = w2i::temp_dir("control");
    auto sp = std::make_shared<Spawns>();
    auto pool = make_pool(dir, sp);
    api::set_worker_pool_control(pool);
    auto status = parse_json_or_throw(call("GET", "/api/worker-pool/status").body);
    CHECK(status["enabled"].get<bool>());
    CHECK(!status["workers"][0]["running"].get<bool>());
    const auto started = call("POST", "/api/worker-pool/worker/con-opus/start");
    CHECK_EQ(started.status, 200);
    CHECK_EQ(started.body, std::string(R"({"ok":true})"));
    CHECK_EQ(call("POST", "/api/worker-pool/worker/ghost/start").status, 404);
    CHECK_EQ(call("POST", "/api/worker-pool/worker/ghost/stop").body, std::string(R"({"ok":false})"));
    CHECK_EQ(call("POST", "/api/worker-pool/worker/con-opus/stop").status, 200);
    auto all = parse_json_or_throw(call("POST", "/api/worker-pool/start").body);
    CHECK(all["ok"].get<bool>());
    CHECK(all["workers"][1]["running"].get<bool>());
    CHECK_EQ(call("POST", "/api/worker-pool/stop").body, std::string(R"({"ok":true})"));
    CHECK_EQ(sp->killed.size(), std::size_t(3));

    const auto page = call("GET", "/api/worker-pool");
    CHECK_EQ(*page.headers.get("Content-Type"), std::string("text/html; charset=UTF-8"));
    for (const auto& a : w2i::load_fixture("web_assets.json")) {
        if (a["name"] != "api/worker-pool-control.html") continue;
        CHECK_EQ(page.body.size(), a["bytes"].get<std::size_t>());
        CHECK_EQ(crypto::sha256_hex(page.body), a["sha256"].get<std::string>());
    }
    api::set_worker_pool_control(nullptr);
}

DI_TEST_MAIN()
