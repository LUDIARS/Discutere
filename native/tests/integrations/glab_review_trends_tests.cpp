// w3-O: GLAB 向けレビュー傾向 (src/integrations/glab-review-trends.ts / src/api/glab-review-trend-routes.ts)。
// 移した TS テスト: tests/flow/glab-review-trends.test.ts。golden: w3o/golden.json の trends / appIds。
// 境界: トークン 31/32 文字・Bearer の形・巨大な Authorization・集計失敗で内部情報を出さない。
#include "w3o/w3o_support.hpp"

#include "api/glab_review_trend_routes.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "integrations/glab_review_trends.hpp"

using namespace di;
using namespace di::integrations;
using di::api::glab_review_trend_routes;
using w3o::golden;

namespace {

StoredReviewSignal signal_from(const Json& j) {
    StoredReviewSignal s;
    if (!j["gameSlug"].is_null()) s.gameSlug = j["gameSlug"].get<std::string>();
    if (!j["appId"].is_null()) s.appId = j["appId"].get<std::int64_t>();
    s.postedAt = j["postedAt"].get<double>();
    if (!j["reaction"].is_null()) s.reaction = j["reaction"].get<std::string>();
    return s;
}

http::Response get(const http::Router& r, const std::string& path, std::optional<std::string> auth = std::nullopt) {
    http::Request req;
    req.method = "GET";
    req.target = path;
    const auto q = path.find('?');
    req.path = path.substr(0, q);
    if (q != std::string::npos) req.query_string = path.substr(q + 1);
    req.remote_address = "127.0.0.1";
    if (auth) req.headers.set("authorization", *auth);
    return r.handle(req);
}

const std::string kToken = "test-glab-review-trends-token-0001";

GameReviewTrend alpha() {
    GameReviewTrend t;
    t.gameSlug = "alpha";
    t.appId = 10;
    t.recentReviewCount = 2;
    t.positivePercent = 50;
    t.latestReviewAt = "2026-08-24T23:59:00.000Z";
    return t;
}

} // namespace

TEST_CASE("golden: analyze_stored_review_trends (7 日窓・slug の trim・appId 一意・並び・limit)") {
    for (const auto& c : golden()["trends"]) {
        std::vector<StoredReviewSignal> records;
        for (const auto& r : c["records"]) records.push_back(signal_from(r));
        Json out = Json::array();
        for (const auto& t : analyze_stored_review_trends(records, c["now"].get<std::int64_t>(), c["limit"].get<int>())) out.push_back(to_json(t));
        W3O_SAME(out, c["result"]);
    }
}

TEST_CASE("golden: steam_app_id_from_source_url") {
    for (const auto& c : golden()["appIds"]) {
        const auto id = steam_app_id_from_source_url(c["url"].get<std::string>());
        const Json actual = id ? Json(*id) : Json(nullptr);
        if (actual != c["appId"]) throw test::Failure("url: " + c["url"].get<std::string>() + " actual " + actual.dump());
    }
}

TEST_CASE("glab-review-trends.test: importer の vote だけを使い、除外済みの発話は省く") {
    const std::int64_t now = *time::parse_iso("2026-08-25T00:00:00.000Z");
    auto db = sql::Database::open(":memory:");
    db->exec(R"(CREATE TABLE utterances (id TEXT PRIMARY KEY, posted_at INTEGER NOT NULL);
                CREATE TABLE reactions (id TEXT PRIMARY KEY, utterance_id TEXT NOT NULL, reaction_type TEXT NOT NULL);
                CREATE TABLE utterance_exclusions (utterance_id TEXT PRIMARY KEY, reason TEXT, created_at INTEGER NOT NULL);)");
    db->run("INSERT INTO utterances (id, posted_at) VALUES (?, ?)", {"ext:steam:r1", now - 60'000});
    db->run("INSERT INTO utterances (id, posted_at) VALUES (?, ?)", {"ext:steam:r2", now - 120'000});
    db->run("INSERT INTO reactions (id, utterance_id, reaction_type) VALUES (?, ?, ?)", {"ext:steam:r1:vote", "ext:steam:r1", "positive"});
    db->run("INSERT INTO reactions (id, utterance_id, reaction_type) VALUES (?, ?, ?)", {"later-negative", "ext:steam:r1", "negative"});
    db->run("INSERT INTO utterance_exclusions (utterance_id, created_at) VALUES (?, ?)", {"ext:steam:r2", now});
    auto attr = [](const std::string& id, const std::string& url) {
        crawler::sources::Attribution a;
        a.utteranceId = id;
        a.source = "steam";
        a.sourceUrl = url;
        a.nativeId = id;
        a.gameSlug = "alpha";
        return a;
    };
    const auto records = read_stored_steam_review_signals(*db, [&] {
        return std::vector<crawler::sources::Attribution>{attr("ext:steam:r1", "https://steamcommunity.com/profiles/1/recommended/10/"),
                                                          attr("ext:steam:r2", "https://steamcommunity.com/profiles/2/recommended/10/"),
                                                          attr("ext:steam:missing", "https://steamcommunity.com/profiles/3/recommended/10/")};
    });
    CHECK_EQ(records.size(), 1u);
    CHECK(records[0] == (StoredReviewSignal{std::string("alpha"), 10, static_cast<double>(now - 60'000), std::string("positive")}));
}

TEST_CASE("glab-review-trends.test: route の 503 / 401 / 200 / 失敗時に内部情報を出さない") {
    // トークン未設定 → 503
    auto unavailable = glab_review_trend_routes([] { return std::vector<GameReviewTrend>{}; }, [] { return std::string(); });
    auto r = get(unavailable, "/integrations/glab/review-trends");
    CHECK_EQ(r.status, 503);
    W3O_SAME(parse_json_or_throw(r.body), (Json{{"ok", false}, {"error", "review_trends_unavailable"}}));
    CHECK_EQ(r.headers.get("cache-control").value_or(""), std::string("private, no-store"));

    http::Router app;
    app.mount("/api", glab_review_trend_routes([] { return std::vector<GameReviewTrend>{alpha()}; }, [] { return kToken; }));
    CHECK_EQ(get(app, "/api/integrations/glab/review-trends").status, 401);
    r = get(app, "/api/integrations/glab/review-trends", "Bearer " + kToken);
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.headers.get("cache-control").value_or(""), std::string("private, no-store"));
    const auto body = parse_json_or_throw(r.body);
    CHECK_EQ(body["source"].get<std::string>(), std::string("discutere"));
    CHECK_EQ(body["windowDays"].get<int>(), 7);
    CHECK_EQ(body["data"][0]["gameSlug"].get<std::string>(), std::string("alpha"));
    CHECK(!str::contains(r.body, "content"));

    auto broken = glab_review_trend_routes([]() -> std::vector<GameReviewTrend> { throw std::runtime_error("private database path"); },
                                           [] { return kToken; });
    r = get(broken, "/integrations/glab/review-trends", "Bearer " + kToken);
    CHECK_EQ(r.status, 503);
    CHECK(!str::contains(r.body, "private database path"));
}

TEST_CASE("境界: トークン長 31/32 文字・Bearer の形・巨大なヘッダ") {
    const std::string t31(31, 'k');
    const std::string t32(32, 'k');
    auto r31 = glab_review_trend_routes([] { return std::vector<GameReviewTrend>{}; }, [t31] { return t31; });
    CHECK_EQ(get(r31, "/integrations/glab/review-trends", "Bearer " + t31).status, 503);
    auto r32 = glab_review_trend_routes([] { return std::vector<GameReviewTrend>{}; }, [t32] { return t32; });
    CHECK_EQ(get(r32, "/integrations/glab/review-trends", "Bearer " + t32).status, 200);
    CHECK_EQ(get(r32, "/integrations/glab/review-trends", "bearer " + t32).status, 401);
    CHECK_EQ(get(r32, "/integrations/glab/review-trends", "Bearer  " + t32).status, 401);
    CHECK_EQ(get(r32, "/integrations/glab/review-trends", "Bearer " + t32 + " x").status, 401);
    CHECK_EQ(get(r32, "/integrations/glab/review-trends", "Bearer " + t32 + "k").status, 401);
    CHECK_EQ(get(r32, "/integrations/glab/review-trends", "Bearer " + t32.substr(1)).status, 401);
    CHECK_EQ(get(r32, "/integrations/glab/review-trends", t32).status, 401);
    CHECK_EQ(get(r32, "/integrations/glab/review-trends", "Bearer " + std::string(1u << 20, 'k')).status, 401);
}

DI_TEST_MAIN()
