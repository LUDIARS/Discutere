// cost_relay のテスト (2/2)。準備は cost_relay_support.hpp。
#include "cost_relay_support.hpp"

using namespace cost_relay_support;

TEST_CASE("relayCostOnce multiplexes and skips empty / no-target") {
    const Json fx = fixture();
    auto db = seeded(fx);
    int calls = 0;
    std::vector<std::string> seen;
    CostFetch fake = [&](const http::ClientRequest& req) {
        ++calls;
        seen.push_back(req.url);
        return ok_response(R"({"recorded":2})");
    };
    std::vector<CostFeedTarget> targets{{"http://a", "/api/cost-feed", std::string("anatomia")},
                                        {"http://c", "/v1/cost-feed", std::string("concordia")}};
    auto r = relay_cost_once(*db, {targets, "discutere", std::nullopt, 1, fake});
    CHECK(r.ok);
    CHECK_EQ(r.pushed, std::size_t{4});
    CHECK_EQ(calls, 2);
    CHECK_EQ(seen, (std::vector<std::string>{"http://a/api/cost-feed", "http://c/v1/cost-feed"}));
    auto empty = relay_cost_once(*db, {targets, "discutere", 1e15, 1, fake});
    CHECK(empty.ok && empty.pushed == 0);
    auto none = relay_cost_once(*db, {{}, "discutere", std::nullopt, 1, fake});
    CHECK(none.ok && none.pushed == 0);
    CHECK_EQ(calls, 2);
}

TEST_CASE("ticker: disabled keeps cursor near-now, cursor advances only on full success") {
    const Json fx = fixture();
    auto db = seeded(fx);
    bool enabled = false;
    bool fail = false;
    int calls = 0;
    std::vector<std::string> errors;
    StartCostRelayOptions o;
    o.intervalMs = 10'000;
    o.resolve = [&] { return CostRelayLive{enabled, {{"http://a", "/api/cost-feed", std::string("anatomia")}}, "discutere"}; };
    o.now = [] { return 1'000'000.0; };
    o.onError = [&](const std::string& m) { errors.push_back(m); };
    o.fetchImpl = [&](const http::ClientRequest&) {
        ++calls;
        auto r = ok_response("{}");
        if (fail) r.status = 502;
        return r;
    };
    CostRelayTicker t(db, o);
    CHECK_EQ(t.next_since(), 1'000'000.0 - 40'000);
    t.tick();
    CHECK_EQ(calls, 0);
    CHECK_EQ(t.next_since(), 990'000.0);
    enabled = true;
    // created_at は 1000〜5000 なので activeSince=990000 では何も送らない
    t.tick();
    CHECK_EQ(calls, 0);
    CostRelayTicker t2(db, [&] {
        auto o2 = o;
        o2.now = [] { return 3000.0; };
        return o2;
    }());
    fail = true;
    t2.tick();
    CHECK(calls >= 1);
    CHECK_EQ(errors.back(), std::string("push to anatomia failed: http 502"));
    CHECK_EQ(t2.next_since(), 3000.0 - 40'000);  // 失敗した window は次 tick で再送
    fail = false;
    t2.tick();
    CHECK_EQ(t2.next_since(), 3000.0 - 10'000);
}

TEST_CASE("start_cost_relay pushes over real HTTP to a loopback server and stops") {
    const Json fx = fixture();
    auto db = seeded(fx);
    std::atomic<int> hits{0};
    auto router = std::make_shared<http::Router>();
    router->post("/api/cost-feed", [&](http::Request& req) {
        auto body = req.json();
        if (body && json_string(*body, "service") == "discutere") ++hits;
        return http::Response::json(Json{{"ok", true}, {"recorded", 4}});
    });
    http::ServerOptions so;
    so.port = 0;
    http::Server server(so, router);
    server.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(server.bound_port());
    StartCostRelayOptions o;
    o.intervalMs = 60'000;
    o.resolve = [&] { return CostRelayLive{true, {{base, "/api/cost-feed", std::nullopt}}, "discutere"}; };
    o.now = [] { return 3000.0; };
    auto stop = start_cost_relay(db, o);
    for (int i = 0; i < 200 && hits.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop();
    CHECK(hits.load() >= 1);
    server.stop();
}

DI_TEST_MAIN()
