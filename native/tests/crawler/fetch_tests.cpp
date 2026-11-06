// fetch のテスト (1/3)。準備は fetch_support.hpp。
#include "fetch_support.hpp"

using namespace fetch_support;

TEST_CASE("niconico search -> watch -> nvComment over fake server") {
    w1d::FakeServer fake;
    std::string threads_body;
    std::mutex m;
    fake.router->get("/api/v2/snapshot/video/contents/search", [](http::Request& r) {
        CHECK_EQ(*r.query("q"), std::string("ゲーム 名"));
        CHECK_EQ(*r.query("_limit"), std::string("2"));
        return json_res(Json{{"data", Json::array({Json{{"contentId", "sm1"}, {"title", "a"}}, Json{{"contentId", "sm2"}}, Json{{"title", "no id"}}})}});
    });
    fake.router->get("/watch/sm1", [&fake](http::Request&) {
        Json nv{{"threadKey", "tk"}, {"server", fake.base}};
        nv["params"] = Json{{"targets", Json::array({Json{{"id", "1"}, {"fork", "main"}}})}};
        Json sr = Json::object();
        sr["data"]["response"]["comment"]["nvComment"] = nv;
        const std::string enc = str::replace_all(json_stringify(sr), "\"", "&quot;");
        return http::Response::html("<meta name=\"server-response\" content=\"" + enc + "\">");
    });
    fake.router->get("/watch/sm2", [](http::Request&) { return http::Response::text("gone", 404); });
    fake.router->post("/v1/threads", [&](http::Request& r) {
        std::lock_guard lock(m);
        threads_body = r.body;
        CHECK_EQ(*r.header("x-frontend-id"), std::string("6"));
        return json_res(Json::parse(R"({"data":{"threads":[{"fork":"owner","comments":[{"id":"o","body":"owner"}]},
          {"fork":"main","comments":[{"id":"c1","body":"いいね","userId":"u1","vposMs":10},{"no":2,"body":"  "},{"no":3,"body":"草"}]}]}})"));
    });
    fake.start();
    NicoFetchOptions o;
    o.gameSlug = "g";
    o.query = "ゲーム 名";
    o.maxVideos = 2;
    o.politeDelayMs = 0;
    o.fetchImpl = w1d::loopback_fetch;
    o.endpoints = {fake.base, fake.base};
    int videos = 0;
    o.onVideo = [&](int v, std::size_t) { videos = v; };
    auto out = fetch_niconico_discussions(o);
    CHECK_EQ(out.size(), std::size_t(2));  // 空白本文は除く、sm2 (404) は致命にしない
    CHECK_EQ(videos, 2);
    CHECK_EQ(out[0].authorId, std::string("nico:u1"));
    CHECK_EQ(out[1].nativeId, std::string("sm1:3"));
    CHECK(str::contains(threads_body, "\"threadKey\":\"tk\""));

    // 既定の fetch は loopback を拒否する (SSRF 対策)
    o.fetchImpl = {};
    CHECK_THROWS(fetch_niconico_discussions(o));
}

TEST_CASE("steam paging, cap, success flag and broken JSON") {
    w1d::FakeServer fake;
    std::atomic<int> calls{0};
    fake.router->get("/appreviews/42", [&](http::Request& r) {
        ++calls;
        const std::string cursor = *r.query("cursor");
        auto review = [](const std::string& id) { return Json{{"recommendationid", id}, {"review", "r" + id}, {"timestamp_created", 1}, {"voted_up", true}}; };
        if (cursor == "*") return json_res(Json{{"success", 1}, {"reviews", Json::array({review("1"), review("2")})}, {"cursor", "C2"}});
        if (cursor == "C2") return json_res(Json{{"success", 1}, {"reviews", Json::array({review("3")})}, {"cursor", "C2"}});
        return json_res(Json{{"success", 1}, {"reviews", Json::array()}});
    });
    fake.router->get("/appreviews/1", [](http::Request&) { return json_res(Json{{"success", 2}}); });
    fake.router->get("/appreviews/2", [](http::Request&) { return http::Response::text("{broken"); });
    fake.router->get("/appreviews/3", [](http::Request&) { return http::Response::text(std::string((8u << 20) + 1, ' ')); });
    fake.start();
    SteamFetchOptions o;
    o.appId = 42;
    o.gameSlug = "g";
    o.politeDelayMs = 0;
    o.fetchImpl = w1d::loopback_fetch;
    o.base = fake.base;
    auto all = fetch_steam_reviews(o);
    CHECK_EQ(all.size(), std::size_t(3));  // cursor が進まなくなったら止まる
    CHECK_EQ(calls.load(), 2);
    o.maxReviews = 2;
    CHECK_EQ(fetch_steam_reviews(o).size(), std::size_t(2));
    o.maxReviews.reset();
    o.appId = 1;
    try {
        fetch_steam_reviews(o);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("steam appreviews success=2"));
    }
    o.appId = 2;
    CHECK_THROWS(fetch_steam_reviews(o));
    o.appId = 3;  // 応答上限 +1
    CHECK_THROWS(fetch_steam_reviews(o));
}

DI_TEST_MAIN()
