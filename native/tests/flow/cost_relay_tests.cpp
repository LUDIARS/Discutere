// cost_relay のテスト (1/2)。準備は cost_relay_support.hpp。
#include "cost_relay_support.hpp"

using namespace cost_relay_support;

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

DI_TEST_MAIN()
