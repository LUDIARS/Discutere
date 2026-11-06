// fetch のテスト (3/3)。準備は fetch_support.hpp。
#include "fetch_support.hpp"

using namespace fetch_support;

TEST_CASE("website fetch: skip failures, size limit, and SSRF block by default") {
    w1d::FakeServer fake;
    fake.router->get("/good", [](http::Request&) {
        std::string p;
        for (int i = 0; i < 10; ++i) p += "とても面白いゲームだと思う。";
        return http::Response::html("<html><body><article><p>" + p + "</p></article></body></html>");
    });
    fake.router->get("/dead", [](http::Request&) { return http::Response::text("x", 404); });
    fake.router->get("/short", [](http::Request&) { return http::Response::html("<article>短い</article>"); });
    fake.router->get("/huge", [](http::Request&) { return http::Response::html(std::string(MAX_HTML_BYTES + 1, 'a')); });
    fake.router->get("/redirect", [](http::Request&) { return http::Response::redirect("http://127.0.0.1:1/x"); });
    fake.start();
    std::vector<std::string> errors;
    WebsiteFetchOptions o;
    o.urls = {fake.base + "/good", fake.base + "/dead", fake.base + "/short", fake.base + "/huge"};
    o.gameSlug = "g";
    o.politeDelayMs = 0;
    o.now = [] { return 42.0; };
    o.fetchImpl = w1d::loopback_fetch;
    o.onError = [&](const std::string& url, const std::string& err) { errors.push_back(url + " " + err); };
    auto items = fetch_website_articles(o);
    CHECK_EQ(items.size(), std::size_t(1));
    CHECK_EQ(items[0].postedAt, 42.0);
    CHECK(str::contains(items[0].content, "面白い"));
    CHECK_EQ(errors.size(), std::size_t(3));

    // 利用者が指定する URL: 既定の fetch は loopback / private を拒否する
    errors.clear();
    o.fetchImpl = {};
    o.urls = {fake.base + "/good", "http://169.254.169.254/latest/meta-data", "http://10.0.0.1/"};
    CHECK(fetch_website_articles(o).empty());
    CHECK_EQ(errors.size(), std::size_t(3));
    for (const auto& e : errors) CHECK(str::contains(e, "blocked") || str::contains(e, "dns"));
}

TEST_CASE("voluptas impressions: anonymity, bearer, https, status handling") {
    using namespace di::flow::user_voices;
    w1d::FakeServer fake;
    std::string seen_auth, seen_limit, seen_game;
    std::mutex m;
    fake.router->get("/api/personas/impressions", [&](http::Request& r) {
        std::lock_guard lock(m);
        seen_auth = r.header("Authorization").value_or("");
        seen_limit = r.query("limit").value_or("");
        seen_game = r.query("game").value_or("");
        if (seen_game == "down") return http::Response::text("x", 503);
        if (seen_game == "nf") return http::Response::text("x", 404);
        if (seen_game == "redir") return http::Response::redirect("http://127.0.0.1:1/");
        if (seen_game == "broken") return http::Response::text("{");
        return json_res(Json::parse(R"({"ok":true,"data":{"impressions":[{"id":"v1","gameTitle":"新作","comment":"楽しい","createdAt":"x"},
           {"id":"v2","gameTitle":"新作","comment":" "},{"id":3,"gameTitle":"x","comment":"y"}]}})"));
    });
    fake.start();
    FetchVoluptasArgs a;
    // loopback は http を許す (localhost 名で到達できる環境に依存しないよう 127.0.0.1 を使う)
    a.baseUrl = fake.base + "/ignored/path?x=1";
    a.bearer = "fake-bearer";
    a.game = "新 作";
    a.gameSlug = "glab-x";
    a.limit = 999;
    auto items = fetch_voluptas_impressions(a);
    CHECK_EQ(items.size(), std::size_t(1));
    CHECK_EQ(items[0].authorId, std::string(GLAB_ANONYMOUS_AUTHOR));
    CHECK(!items[0].authorName.has_value());
    CHECK_EQ(seen_auth, std::string("Bearer fake-bearer"));
    CHECK_EQ(seen_limit, std::string("200"));
    CHECK_EQ(seen_game, std::string("新 作"));
    a.limit = 0;
    fetch_voluptas_impressions(a);
    CHECK_EQ(seen_limit, std::string("1"));

    a.game = "down";
    CHECK_THROWS(([&] {
        try {
            fetch_voluptas_impressions(a);
        } catch (const VoluptasUnavailable&) {
            throw;
        } catch (...) {
        }
    }()));
    a.game = "nf";
    try {
        fetch_voluptas_impressions(a);
        CHECK(false);
    } catch (const VoluptasUnavailable&) {
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("Voluptas impressions request failed with status 404"));
        CHECK(!str::contains(e.what(), "fake-bearer"));
    }
    a.game = "redir";  // redirect: manual → 3xx は失敗
    CHECK_THROWS(fetch_voluptas_impressions(a));
    a.game = "broken";
    CHECK_THROWS(fetch_voluptas_impressions(a));

    a.baseUrl = "http://example.com";
    try {
        fetch_voluptas_impressions(a);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(str::contains(e.what(), "HTTPS"));
    }
    a.baseUrl = "not a url";
    CHECK_THROWS(fetch_voluptas_impressions(a));
    a.baseUrl = "http://127.0.0.1:1";  // 接続できない → VoluptasUnavailable
    bool unavailable = false;
    try {
        fetch_voluptas_impressions(a);
    } catch (const VoluptasUnavailable&) {
        unavailable = true;
    }
    CHECK(unavailable);
}

DI_TEST_MAIN()
