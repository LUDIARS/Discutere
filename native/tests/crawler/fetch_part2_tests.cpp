// fetch のテスト (2/3)。準備は fetch_support.hpp。
#include "fetch_support.hpp"

using namespace fetch_support;

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

DI_TEST_MAIN()
