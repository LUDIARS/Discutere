// crawler/sources と Voluptas client のネットワーク部分を偽サーバで確かめる
// (tests/crawler/external/{youtube-fetch,website}.test.ts, tests/flow/user-voices.test.ts の Voluptas 部分の観点 + 境界)。
#include <atomic>
#include <mutex>

#include "base/strings.hpp"
#include "check.hpp"
#include "crawler/sources/niconico.hpp"
#include "crawler/sources/reddit.hpp"
#include "crawler/sources/steam.hpp"
#include "crawler/sources/website.hpp"
#include "crawler/sources/youtube_comments.hpp"
#include "crawler/sources/youtube_videos.hpp"
#include "flow/user_voices/voluptas_impressions.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::crawler::sources;

namespace {
http::Response json_res(const Json& j, int status = 200) { return http::Response::json(j, status); }
} // namespace

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

TEST_CASE("reddit oauth, search and comments without leaking credentials") {
    w1d::FakeServer fake;
    fake.router->post("/api/v1/access_token", [](http::Request& r) {
        // Basic base64("id:fake-secret")
        if (*r.header("Authorization") != "Basic aWQ6ZmFrZS1zZWNyZXQ=") return json_res(Json{{"error", "x"}}, 401);
        CHECK_EQ(r.body, std::string("grant_type=client_credentials"));
        return json_res(Json{{"access_token", "tok-123"}});
    });
    fake.router->get("/r/sub/search", [](http::Request& r) {
        CHECK_EQ(*r.header("Authorization"), std::string("Bearer tok-123"));
        CHECK_EQ(*r.query("restrict_sr"), std::string("1"));
        return json_res(Json::parse(R"({"data":{"children":[{"data":{"id":"t1","title":"T","subreddit":"sub","num_comments":2}},{"data":{"title":"no id"}}]}})"));
    });
    fake.router->get("/comments/t1", [](http::Request&) {
        return json_res(Json::parse(R"([{},{"data":{"children":[{"kind":"t1","data":{"name":"t1_a","id":"a","body":"hi","author":"u","created_utc":1}}]}}])"));
    });
    fake.start();
    RedditFetchOptions o;
    o.clientId = "id";
    o.clientSecret = "fake-secret";
    o.userAgent = "test";
    o.gameSlug = "g";
    o.query = "q";
    o.subreddit = "sub";
    o.politeDelayMs = 0;
    o.fetchImpl = w1d::loopback_fetch;
    o.endpoints = {fake.base, fake.base + "/api/v1/access_token"};
    auto out = fetch_reddit_discussions(o);
    CHECK_EQ(out.size(), std::size_t(1));
    CHECK_EQ(out[0].sourceUrl, std::string("https://www.reddit.com/comments/t1/a/"));  // permalink 既定 /comments/<id>/
    o.clientSecret = "wrong-secret";
    try {
        fetch_reddit_discussions(o);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(!str::contains(e.what(), "wrong-secret"));
        CHECK_EQ(std::string(e.what()), std::string("reddit token HTTP 401"));
    }
}

TEST_CASE("youtube comments pagination, quota, disabled, cap; key not in errors") {
    w1d::FakeServer fake;
    std::atomic<int> calls{0};
    auto thread = [](const std::string& id, bool reply) {
        Json t{{"snippet", {{"topLevelComment", {{"id", id}, {"snippet", {{"textOriginal", "c" + id}}}}}}}};
        if (reply) t["replies"] = Json{{"comments", Json::array({Json{{"id", id + "1"}, {"snippet", {{"textOriginal", "r"}}}}})}};
        return t;
    };
    fake.router->get("/commentThreads", [&](http::Request& r) {
        const std::string vid = *r.query("videoId");
        CHECK_EQ(*r.query("key"), std::string("fake-key"));
        if (vid == "X") return http::Response::json(Json{{"error", {{"errors", Json::array({Json{{"reason", "commentsDisabled"}}})}}}}, 403);
        if (vid == "E") return http::Response::text("server error", 500);
        if (vid == "M") return json_res(Json{{"items", Json::array({thread("1", false), thread("2", false), thread("3", false)})}});
        ++calls;
        if (!r.query("pageToken")) return json_res(Json{{"items", Json::array({thread("a", true), thread("b", false)})}, {"nextPageToken", "PAGE2"}});
        CHECK_EQ(*r.query("pageToken"), std::string("PAGE2"));
        return json_res(Json{{"items", Json::array({thread("c", false)})}});
    });
    fake.router->get("/search", [](http::Request&) {
        return json_res(Json::parse(R"({"items":[{"id":{"videoId":"v1"},"snippet":{"title":"t"}},{"id":{}}]})"));
    });
    fake.router->get("/channels", [](http::Request&) { return json_res(Json::parse(R"({"items":[{"contentDetails":{"relatedPlaylists":{"uploads":"UU1"}}}]})")); });
    fake.router->get("/playlistItems", [](http::Request& r) {
        CHECK_EQ(*r.query("playlistId"), std::string("UU1"));
        return json_res(Json::parse(R"({"items":[{"contentDetails":{"videoId":"p1"}},{"snippet":{"resourceId":{"videoId":"p2"}}}]})"));
    });
    fake.start();
    YoutubeCommentsOptions o;
    o.videoId = "VID";
    o.gameSlug = "vs";
    o.apiKey = "fake-key";
    o.quota = create_quota_tracker();
    o.fetchImpl = w1d::loopback_fetch;
    o.apiBase = fake.base;
    auto items = fetch_video_comments(o);
    CHECK_EQ(calls.load(), 2);
    CHECK_EQ(items.size(), std::size_t(4));
    CHECK_EQ(o.quota->spent(), 2LL);
    CHECK_EQ(*items[1].parentNativeId, std::string("a"));

    o.videoId = "X";
    CHECK(fetch_video_comments(o).empty());
    o.videoId = "M";
    o.maxComments = 2;
    CHECK_EQ(fetch_video_comments(o).size(), std::size_t(2));
    o.videoId = "E";
    try {
        fetch_video_comments(o);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(!str::contains(e.what(), "fake-key"));
    }
    // quota が尽きていれば取りに行かない
    o.videoId = "VID";
    o.quota = create_quota_tracker(0);
    CHECK(fetch_video_comments(o).empty());

    SearchDiscoverOptions s;
    s.apiKey = "fake-key";
    s.fetchImpl = w1d::loopback_fetch;
    s.apiBase = fake.base;
    s.query = "q";
    s.quota = create_quota_tracker(1000);
    auto vids = discover_videos_by_search(s);
    CHECK_EQ(vids.size(), std::size_t(1));
    CHECK_EQ(s.quota->spent(), 100LL);
    ChannelDiscoverOptions c;
    c.apiKey = "fake-key";
    c.fetchImpl = w1d::loopback_fetch;
    c.apiBase = fake.base;
    c.channelId = "UC1";
    auto pl = discover_videos_by_channel(c);
    CHECK_EQ(pl.size(), std::size_t(2));
    CHECK_EQ(pl[1].videoId, std::string("p2"));
}

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
