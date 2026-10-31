#include "check.hpp"

#include <thread>

#include "infra/http.hpp"
#include "infra/http_client.hpp"
#include "infra/http_server.hpp"

using namespace di;

TEST_CASE("router matches Hono patterns") {
    std::map<std::string, std::string> p;
    CHECK(http::Router::match("/a/:id", "/a/42", &p));
    CHECK_EQ(p["id"], std::string("42"));
    CHECK(!http::Router::match("/a/:id", "/a/", nullptr));
    CHECK(!http::Router::match("/a", "/a/", nullptr));
    CHECK(http::Router::match("/api/*", "/api", nullptr));
    CHECK(http::Router::match("/api/*", "/api/x/y", nullptr));
    CHECK(!http::Router::match("/api/*", "/apix", nullptr));
    CHECK(http::Router::match("/admin/x*", "/admin/x", nullptr));
    CHECK(http::Router::match("/admin/x*", "/admin/xyz/1", nullptr));
    CHECK(http::Router::match("*", "/anything", nullptr));
}

TEST_CASE("middleware order and mount follow Hono") {
    http::Router app;
    std::vector<std::string> trace;
    app.use("*", [&](http::Request& r, const http::Next& next) {
        trace.push_back("cors");
        auto res = next();
        res.headers.set("X-Cors", "1");
        (void)r;
        return res;
    });
    app.get("/health", [](http::Request&) { return http::Response::json(Json{{"status", "ok"}}); });
    app.use("/api/*", [&](http::Request& r, const http::Next& next) {
        trace.push_back("auth");
        r.vars["userRole"] = r.from_loopback() ? "admin" : "general";
        return next();
    });
    http::Router sub;
    sub.get("/items/:id", [](http::Request& r) { return http::Response::json(Json{{"id", r.param("id")}, {"role", r.var("userRole")}}); });
    app.mount("/api", sub);

    http::Request req;
    req.method = "GET";
    req.path = "/api/items/a%20b";
    req.remote_address = "127.0.0.1";
    auto res = app.handle(req);
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, std::string("{\"id\":\"a b\",\"role\":\"admin\"}"));
    CHECK_EQ(*res.headers.get("x-cors"), std::string("1"));
    CHECK_EQ(trace.size(), std::size_t(2));

    trace.clear();
    http::Request health;
    health.method = "HEAD";
    health.path = "/health";
    auto h = app.handle(health);
    CHECK_EQ(h.status, 200);
    CHECK(h.body.empty());
    CHECK_EQ(trace.size(), std::size_t(1));

    http::Request missing;
    missing.method = "GET";
    missing.path = "/nope";
    auto m = app.handle(missing);
    CHECK_EQ(m.status, 404);
    CHECK_EQ(m.body, std::string("404 Not Found"));
}

TEST_CASE("body limits per route") {
    http::Router app;
    app.get("/a", [](http::Request&) { return http::Response::text("a"); });
    app.post("/upload", [](http::Request&) { return http::Response::text("u"); }, {8u << 20});
    http::Router sub;
    sub.body_limit("*", 160'000);
    sub.post("/x", [](http::Request&) { return http::Response::text("x"); });
    app.mount("/internal/ext", sub);
    CHECK_EQ(app.max_body_for("POST", "/upload"), std::size_t(8u << 20));
    CHECK_EQ(app.max_body_for("POST", "/internal/ext/x"), std::size_t(160'000));
    CHECK_EQ(app.max_body_for("GET", "/a"), http::kDefaultMaxBody);
}

TEST_CASE("cors preflight") {
    http::Router app;
    app.use("*", http::cors("http://localhost:5173", true, {"GET", "POST"}, {"Content-Type"}));
    app.get("/x", [](http::Request&) { return http::Response::text("x"); });
    http::Request pre;
    pre.method = "OPTIONS";
    pre.path = "/x";
    auto r = app.handle(pre);
    CHECK_EQ(r.status, 204);
    CHECK_EQ(*r.headers.get("Access-Control-Allow-Origin"), std::string("http://localhost:5173"));
    CHECK_EQ(*r.headers.get("Access-Control-Allow-Credentials"), std::string("true"));
    CHECK_EQ(*r.headers.get("Access-Control-Allow-Methods"), std::string("GET,POST"));
}

TEST_CASE("server and client over loopback with body limit boundary") {
    auto router = std::make_shared<http::Router>();
    router->get("/health", [](http::Request&) { return http::Response::json(Json{{"status", "ok"}, {"service", "discutere"}}); });
    router->post("/echo", [](http::Request& r) {
        return http::Response::json(Json{{"len", r.body.size()}, {"loopback", r.from_loopback()}});
    }, {1000});
    http::ServerOptions o;
    o.host = "127.0.0.1";
    o.port = 0;
    http::Server server(o, router);
    server.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(server.bound_port());

    http::ClientRequest blocked;
    blocked.url = base + "/health";
    auto b = http::fetch(blocked);
    CHECK(!b.transport_ok);  // 既定では loopback へ出ない (SSRF 対策)
    CHECK(b.error.find("blocked") != std::string::npos);

    http::ClientRequest get;
    get.url = base + "/health";
    get.allow_private_network = true;
    auto g = http::fetch(get);
    CHECK(g.ok());
    CHECK_EQ(g.body, std::string("{\"status\":\"ok\",\"service\":\"discutere\"}"));

    http::ClientRequest post;
    post.method = "POST";
    post.url = base + "/echo";
    post.allow_private_network = true;
    post.body = std::string(1000, 'a');
    auto p = http::fetch(post);
    if (!p.transport_ok) std::cerr << "post error: " << p.error << "\n";
    CHECK_EQ(p.status, 200);
    CHECK_EQ(p.body, std::string("{\"len\":1000,\"loopback\":true}"));
    post.body = std::string(1001, 'a');
    auto big = http::fetch(post);
    CHECK_EQ(big.status, 413);

    http::ClientRequest limited;
    limited.url = base + "/health";
    limited.allow_private_network = true;
    limited.max_response_bytes = 10;
    auto l = http::fetch(limited);
    CHECK(!l.transport_ok);

    http::ClientRequest nf;
    nf.url = base + "/missing";
    nf.allow_private_network = true;
    CHECK_EQ(http::fetch(nf).status, 404);
    server.stop();
}

TEST_CASE("public address classification") {
    CHECK(!http::is_public_address("127.0.0.1"));
    CHECK(!http::is_public_address("10.1.2.3"));
    CHECK(!http::is_public_address("172.20.0.1"));
    CHECK(!http::is_public_address("192.168.0.1"));
    CHECK(!http::is_public_address("169.254.169.254"));
    CHECK(!http::is_public_address("100.64.0.1"));
    CHECK(!http::is_public_address("::1"));
    CHECK(!http::is_public_address("fd00::1"));
    CHECK(!http::is_public_address("::ffff:10.0.0.1"));
    CHECK(http::is_public_address("8.8.8.8"));
    CHECK(http::is_public_address("2606:4700::1111"));
}

DI_TEST_MAIN()
