// src/flow/cost-report.ts / cost-relay.ts の移植の確認 (tests/flow/cost-relay.test.ts の観点 + golden)。
// llm_call_log は w1-C が作る flow の DB スキーマと同じ列をテスト内で合成する。
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "flow/cost_relay.hpp"
#include "flow/cost_report.hpp"
#include "infra/http_server.hpp"

using namespace di;
using namespace di::flow;

namespace {

Json fixture() {
    auto text = fs::read_file(test::fixtures_dir() + "/flow/w1e_cost.json");
    if (!text) throw test::Failure("fixture missing");
    return parse_json_or_throw(*text, {16u << 20, 1'000'000, 64});
}

sql::DatabasePtr seeded(const Json& fx) {
    auto db = sql::Database::open(":memory:");
    db->exec(fx["schema"].get<std::string>());
    for (const auto& r : fx["rows"]) {
        sql::Params p;
        for (const auto& v : r) {
            if (v.is_null()) p.add(nullptr);
            else if (v.is_string()) p.add(v.get<std::string>());
            else if (v.is_number_integer()) p.add(v.get<std::int64_t>());
            else p.add(v.get<double>());
        }
        db->run("INSERT INTO llm_call_log (flow, session_id, model, backend, input_tokens, output_tokens, "
                "cache_read_input_tokens, cache_creation_input_tokens, cost_usd, created_at, location) VALUES (?,?,?,?,?,?,?,?,?,?,'loc')",
                p);
    }
    return db;
}

CostReportFilter filter_of(const Json& f) {
    CostReportFilter out;
    if (f.contains("sessionId")) out.sessionId = f["sessionId"].get<std::string>();
    if (f.contains("since")) out.since = f["since"].get<double>();
    if (f.contains("activeSince")) out.activeSince = f["activeSince"].get<double>();
    return out;
}

http::ClientResponse ok_response(std::string body) {
    http::ClientResponse r;
    r.transport_ok = true;
    r.status = 200;
    r.body = std::move(body);
    return r;
}

std::vector<CostFeedRow> sample_rows() {
    CostFeedRow r;
    r.sessionId = "S1";
    r.model = "opus";
    r.backend = "claude-cli";
    r.calls = 2;
    r.inputTokens = 20;
    r.outputTokens = 8;
    r.cacheReadTokens = 200;
    r.cacheCreationTokens = 40;
    r.costUsd = 0.03;
    return {r};
}

} // namespace

TEST_CASE("summaries, feed rows and the CLI text match Node") {
    const Json fx = fixture();
    auto db = seeded(fx);
    for (const auto& c : fx["cases"]) {
        const auto f = filter_of(c["filter"]);
        const auto report = summarize_cost(*db, f);
        CHECK_EQ(json_stringify(to_json(report)), json_stringify(c["report"]));
        Json feed = Json::array();
        for (const auto& r : cost_feed_rows(*db, f)) feed.push_back(to_json(r));
        CHECK_EQ(json_stringify(feed), json_stringify(c["feed"]));
        CHECK_EQ(format_cost_report(report), c["text"].get<std::string>());
    }
}

TEST_CASE("pushCostFeed builds endpoint + body, folds failures") {
    std::string url, body;
    CostFetch fake = [&](const http::ClientRequest& req) {
        url = req.url;
        body = req.body;
        CHECK(req.allow_private_network);
        CHECK_EQ(req.max_response_bytes, kCostFeedMaxResponseBytes);
        return ok_response(R"({"ok":true,"recorded":1})");
    };
    auto res = push_cost_feed({"http://anatomia:4200/", "/api/cost-feed", std::string("anatomia")}, "discutere", sample_rows(), 12345, fake);
    CHECK(res.ok);
    CHECK_EQ(*res.recorded, 1.0);
    CHECK_EQ(url, std::string("http://anatomia:4200/api/cost-feed"));
    CHECK_EQ(body, std::string(R"({"service":"discutere","ts":12345,"sessions":[{"sessionId":"S1","model":"opus","backend":"claude-cli","calls":2,"inputTokens":20,"outputTokens":8,"cacheReadTokens":200,"cacheCreationTokens":40,"costUsd":0.03}]})"));
    push_cost_feed({"http://concordia:17330", "/v1/cost-feed", std::nullopt}, "discutere", sample_rows(), 1, fake);
    CHECK_EQ(url, std::string("http://concordia:17330/v1/cost-feed"));
    // 本文が JSON でなくても ok (recorded なし)
    auto plain = push_cost_feed({"http://x", "/p", std::nullopt}, "s", sample_rows(), 1, [](const http::ClientRequest&) { return ok_response("nope"); });
    CHECK(plain.ok && !plain.recorded);
    auto bad = push_cost_feed({"http://x", "/p", std::nullopt}, "s", sample_rows(), 1, [](const http::ClientRequest&) {
        auto r = ok_response("");
        r.status = 500;
        return r;
    });
    CHECK(!bad.ok);
    CHECK_EQ(*bad.error, std::string("http 500"));
    auto threw = push_cost_feed({"http://x", "/p", std::nullopt}, "s", sample_rows(), 1,
                                [](const http::ClientRequest&) -> http::ClientResponse { throw std::runtime_error("boom"); });
    CHECK(!threw.ok);
    CHECK_EQ(*threw.error, std::string("boom"));
    auto transport = push_cost_feed({"http://x", "/p", std::nullopt}, "s", sample_rows(), 1, [](const http::ClientRequest&) {
        http::ClientResponse r;
        r.error = "timeout";
        return r;
    });
    CHECK(!transport.ok);
    CHECK_EQ(*transport.error, std::string("timeout"));
}

TEST_CASE("buildCostFeedTargets") {
    CHECK(build_cost_feed_targets({}).empty());
    const auto both = build_cost_feed_targets({std::string("http://a"), std::string("http://c")});
    CHECK_EQ(both.size(), std::size_t{2});
    CHECK_EQ(both[0].path, std::string("/api/cost-feed"));
    CHECK_EQ(both[1].path, std::string("/v1/cost-feed"));
    const auto only = build_cost_feed_targets({std::nullopt, std::string("http://c")});
    CHECK_EQ(*only[0].label, std::string("concordia"));
}

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
