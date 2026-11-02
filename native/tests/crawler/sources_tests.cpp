// crawler/sources の純関数 (map 系・抽出) のテスト。Node の出力 (fixtures/crawler/*.json) と、
// tests/crawler/external/{steam-map,reddit,youtube-map,niconico-map,persona,website}.test.ts の観点を移す。
#include <cmath>

#include "base/strings.hpp"
#include "check.hpp"
#include "crawler/sources/ingested_store.hpp"
#include "crawler/sources/niconico.hpp"
#include "crawler/sources/persona.hpp"
#include "crawler/sources/raw_store.hpp"
#include "crawler/sources/reddit.hpp"
#include "crawler/sources/steam.hpp"
#include "crawler/sources/website.hpp"
#include "crawler/sources/youtube_comments.hpp"
#include "crawler/sources/youtube_quota.hpp"
#include "crawler/sources/youtube_videos.hpp"
#include "flow/user_voices/voluptas_impressions.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::crawler::sources;

TEST_CASE("persona labels match Node and never leak the anchor") {
    const Json f = w1d::load_fixture("crawler/persona.json");
    for (const auto& c : f["labels"]) CHECK_EQ(masked_persona_label(c["input"].get<std::string>()), c["output"].get<std::string>());
    CHECK_EQ(to_speaker_id("steam", "765"), f["speakerId"].get<std::string>());
    w1d::check_json_eq(to_json(masked_speaker_with_source("ext:steam:1", SourceAttribution{"steam", "https://s/"})), f["masked"][0], "masked");
    w1d::check_json_eq(to_json(masked_speaker_with_source("ext:steam:1", std::nullopt)), f["masked"][1], "masked null");
    const auto exposed = masked_speaker_with_source(to_speaker_id("steam", "76561199058015945"), std::nullopt);
    CHECK(str::starts_with(exposed.speaker, "論者#"));
    CHECK(!str::contains(json_stringify(to_json(exposed)), "76561199058015945"));
    CHECK(!source_badge(std::nullopt).has_value());
}

TEST_CASE("mappers match Node golden") {
    const Json f = w1d::load_fixture("crawler/sources.json");
    NicoComment c1;
    c1.id = "c1";
    c1.body = "888";
    c1.postedAt = "2026-07-12T00:00:00.000Z";
    c1.userId = "u";
    c1.vposMs = 45678;
    w1d::check_json_eq(to_json(map_nico_comment("sm9", "g", c1)), f["nico"][0], "nico 0");
    NicoComment c2;
    c2.no = 12;
    c2.body = "x";
    w1d::check_json_eq(to_json(map_nico_comment("sm9", "g", c2)), f["nico"][1], "nico 1");
    NicoComment c3;
    c3.body = "x";
    c3.postedAt = "";
    w1d::check_json_eq(to_json(map_nico_comment("sm9", "g", c3)), f["nico"][2], "nico 2");

    auto s1 = steam_review_from_json(Json::parse(R"({"recommendationid":"1","author":{"steamid":"765"},"review":"r","language":"japanese","timestamp_created":1779333194,"voted_up":true,"votes_up":3})"));
    w1d::check_json_eq(to_json(map_steam_review(1794680, "vs", *s1)), f["steam"][0], "steam 0");
    auto s2 = steam_review_from_json(Json::parse(R"({"recommendationid":"2","review":"r","timestamp_created":1,"voted_up":false})"));
    w1d::check_json_eq(to_json(map_steam_review(1, "x", *s2)), f["steam"][1], "steam 1");
    CHECK(!steam_review_from_json(Json::parse(R"({"review":"r"})")).has_value());

    auto r1 = map_reddit_comment(reddit_comment_from_json(Json::parse(R"({"name":"t1_c1","id":"c1","body":"b","author":"w","created_utc":1700000000,"parent_id":"t3_a","ups":42})")),
                                 {"a", "g", std::string("/r/x/comments/a/t/")});
    w1d::check_json_eq(to_json(*r1), f["reddit"][0], "reddit 0");
    auto r2 = map_reddit_comment(reddit_comment_from_json(Json::parse(R"({"id":"t1_c2","body":"b","author":"[deleted]","parent_id":"t1_c1"})")), {"a", "g", std::nullopt});
    w1d::check_json_eq(to_json(*r2), f["reddit"][1], "reddit 1");
    CHECK(f["reddit"][2].is_null());
    CHECK(!map_reddit_comment(reddit_comment_from_json(Json::parse(R"({"body":"b"})")), {"a", "g", std::nullopt}).has_value());

    const Json tree = Json::parse(R"([
      {"kind":"t1","data":{"name":"t1_a","id":"a","body":"p","author":"u1","created_utc":1,"replies":{"data":{"children":[{"kind":"t1","data":{"name":"t1_b","id":"b","body":"c","created_utc":2,"parent_id":"t1_a"}},{"kind":"more","data":{}}]}}}},
      {"kind":"t1","data":{"name":"t1_c","id":"c","body":"q","created_utc":3,"replies":""}}])");
    auto flat = flatten_comments(tree, {"s", "g", std::nullopt}, 10);
    Json flat_json = Json::array();
    for (const auto& u : flat) flat_json.push_back(to_json(u));
    w1d::check_json_eq(flat_json, f["redditFlatten"], "reddit flatten (順序も Node と同じ)");
    CHECK_EQ(flatten_comments(tree, {"s", "g", std::nullopt}, 1).size(), std::size_t(1));

    const Json thread = Json::parse(R"({"snippet":{"topLevelComment":{"id":"top","snippet":{"textDisplay":"d","authorDisplayName":"t","authorChannelId":{"value":"UC1"},"publishedAt":"2024-02-01T00:00:00Z","likeCount":12}}},
      "replies":{"comments":[{"id":"r1","snippet":{"textOriginal":"o"}}]}})");
    Json yt = Json::array();
    for (const auto& u : map_comment_thread(thread, "VID", "g")) yt.push_back(to_json(u));
    w1d::check_json_eq(yt, f["youtube"], "youtube thread");
    CHECK(map_comment_thread(Json::parse("{}"), "V", "g").empty());

    w1d::check_json_eq(to_json(*map_search_video(Json::parse(R"({"id":{"videoId":"abc"},"snippet":{"title":"t","channelId":"UC","publishedAt":"2024-01-02T03:04:05Z"}})"))),
                       f["videos"][0], "search video");
    CHECK(!map_search_video(Json::parse(R"({"id":{}})")).has_value());
    w1d::check_json_eq(to_json(*map_playlist_video(Json::parse(R"({"snippet":{"resourceId":{"videoId":"v9"}}})"))), f["videos"][2], "playlist video");

    flow::user_voices::VoluptasImpression imp{"v 1", "新作", false, std::nullopt, "c", "2026-10-01T00:00:00Z"};
    w1d::check_json_eq(to_json(flow::user_voices::map_impression("glab-x", imp)), f["impression"], "impression");
}

TEST_CASE("extract_nv_comment matches Node golden") {
    const Json f = w1d::load_fixture("crawler/sources.json");
    for (const auto& c : f["nv"]) {
        auto nv = extract_nv_comment(c["input"].get<std::string>());
        if (c["output"].is_null()) {
            CHECK(!nv.has_value());
        } else {
            CHECK(nv.has_value());
            CHECK_EQ(nv->threadKey, c["output"]["threadKey"].get<std::string>());
            CHECK_EQ(nv->server, c["output"]["server"].get<std::string>());
            w1d::check_json_eq(nv->params, c["output"]["params"], "params");
        }
    }
    // 巨大・壊れた入力でも落ちない
    CHECK(!extract_nv_comment(std::string(1u << 20, '<')).has_value());
    CHECK(!extract_nv_comment("<meta name=\"server-response\" content=\"" + std::string(9u << 20, '[') + "\">").has_value());
}

TEST_CASE("website extract and normalize match Node golden") {
    const Json f = w1d::load_fixture("crawler/website.json");
    for (const auto& c : f["extract"]) {
        const auto a = extract_article(c["input"].get<std::string>());
        const Json& out = c["output"];
        CHECK_EQ(a.title, out["title"].get<std::string>());
        CHECK_EQ(a.author.has_value(), out.contains("author"));
        if (a.author) CHECK_EQ(*a.author, out["author"].get<std::string>());
        // 本文は str::strip_html (線形) を使うので改行は空白に詰まる (Node は段落ごとに改行)。空白を詰めて比べる。
        const std::string node_text = out["text"].get<std::string>();
        if (str::js_length(node_text) < MAX_CONTENT_CHARS) CHECK_EQ(a.text, str::squish(node_text));
        else CHECK_EQ(a.text, node_text);
    }
    for (const auto& c : f["normalize"]) CHECK_EQ(normalize_url(c["input"].get<std::string>()), c["output"].get<std::string>());
    const auto art = extract_article(f["extract"][0]["input"].get<std::string>());
    Json m0 = to_json(map_website_article({"https://www.example.com/review?utm_medium=mail#c", "g", 1, art}));
    Json e0 = f["map"][0];
    // content は本文の改行の差を除いて同じ
    {
        const std::string ec = e0["content"].get<std::string>();
        const auto pos = ec.find("\n\n");
        CHECK_EQ(m0["content"].get<std::string>(), ec.substr(0, pos + 2) + str::squish(ec.substr(pos + 2)));
    }
    m0.erase("content");
    e0.erase("content");
    w1d::check_json_eq(m0, e0, "website map");
    w1d::check_json_eq(to_json(map_website_article({"https://blog.test/p", "g", 1, {"", "english text", std::nullopt}})), f["map"][1], "map 1");
    w1d::check_json_eq(to_json(map_website_article({"https://blog.test/p", "g", 1, {"T", "x", std::string("")}})), f["map"][2], "map 2");
}

TEST_CASE("website extract is linear on hostile input") {
    const auto t0 = std::chrono::steady_clock::now();
    std::string hostile;
    for (int i = 0; i < 40'000; ++i) hostile += "<meta <script <nav <!-- <article ";
    auto a = extract_article(hostile);
    (void)a;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 10'000);
    // 本文上限ちょうど / +1
    auto exact = extract_article("<article>" + std::string(MAX_CONTENT_CHARS, 'a') + "</article>");
    CHECK_EQ(exact.text.size(), MAX_CONTENT_CHARS);
    auto over = extract_article("<article>" + std::string(MAX_CONTENT_CHARS + 1, 'a') + "</article>");
    CHECK_EQ(str::js_length(over.text), MAX_CONTENT_CHARS);
    CHECK(str::ends_with(over.text, "…"));
}

TEST_CASE("youtube quota") {
    auto q = create_quota_tracker(250);
    CHECK_EQ(q->remaining(), 250LL);
    q->spend(YT_COST.search);
    CHECK_EQ(q->spent(), 100LL);
    CHECK(q->can_spend(100));
    q->spend(YT_COST.search);
    CHECK(!q->can_spend(100));
    try {
        q->spend(YT_COST.search);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(str::contains(e.what(), "quota exceeded"));
    }
    q->spend(YT_COST.list);
    CHECK_EQ(q->spent(), 201LL);
}

TEST_CASE("ingested and raw stores") {
    const std::string dir = w1d::temp_dir("stores");
    auto ing = open_ingested_store(dir + "/sub/.ingested.sqlite");
    CHECK(!ing->has("steam", "1"));
    ing->add("steam", "1", 5);
    ing->add("steam", "1");
    CHECK(ing->has("steam", "1"));
    CHECK(!ing->has("youtube", "1"));
    ing->close();
    auto raw = open_raw_store(dir + "/.raw.sqlite");
    CHECK(!raw->get("u1").has_value());
    raw->set({"u1", "website", "https://x", "full text", 7});
    raw->set({"u1", "website", "https://y", "full text 2", 8});
    auto d = raw->get("u1");
    CHECK(d.has_value());
    CHECK_EQ(d->url, std::string("https://y"));
    CHECK_EQ(d->rawText, std::string("full text 2"));
    CHECK_EQ(d->savedAt, std::int64_t(8));
    raw->close();
}

DI_TEST_MAIN()
