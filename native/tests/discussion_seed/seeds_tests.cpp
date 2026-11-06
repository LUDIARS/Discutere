// src/discussion-seed/{genres,store-trends}.ts の移植テスト。
// golden: native/tests/golden/w1-A-discussion_seed.gen.ts (store-trends は偽の Steam 応答)
#include <atomic>
#include <memory>
#include <thread>

#include "../check.hpp"
#include "base/fsutil.hpp"
#include "discussion_seed/genres.hpp"
#include "discussion_seed/store_trends.hpp"
#include "infra/http_server.hpp"

using namespace di;
using namespace di::discussion_seed;

namespace {
Json fixture() {
    auto text = fs::read_file(test::fixtures_dir() + "/discussion_seed/seeds.json");
    if (!text) throw std::runtime_error("fixture missing");
    return parse_json_or_throw(*text, JsonLimits{8u << 20, 1'000'000, 64});
}
Json topics_json(const std::vector<SeedTopic>& v) {
    Json a = Json::array();
    for (const auto& t : v) a.push_back(to_json(t));
    return a;
}
}  // namespace

TEST_CASE("golden: ジャンル議題・巡回選択") {
    const Json fx = fixture();
    std::vector<SeedTopic> topics;
    for (const auto& s : GENRE_SEEDS()) topics.push_back(build_genre_topic(s));
    CHECK_EQ(json_stringify(topics_json(topics)), json_stringify(fx["genreTopics"]));
    CHECK_EQ(json_stringify(to_json(build_genre_topic({"テスト", std::nullopt}))), json_stringify(fx["noAngle"]));
    for (const auto& p : fx["picks"]) CHECK_EQ(pick_genre_seed(p["index"].get<std::int64_t>()).genre, p["genre"].get<std::string>());
    CHECK_THROWS(pick_genre_seed(0, {}));
}

TEST_CASE("golden: ストアトレンド (偽の Steam 応答)") {
    const Json fx = fixture();
    auto current = std::make_shared<Json>();
    auto router = std::make_shared<http::Router>();
    router->get("/api/featuredcategories/", [current](http::Request&) {
        const Json& r = *current;
        if (r.is_null()) return http::Response::text("err", 500);
        return http::Response::bytes(r.get<std::string>(), "application/json");
    });
    http::ServerOptions o;
    o.port = 0;
    http::Server server(o, router);
    server.start();
    StoreTrendSource src;
    src.url = "http://127.0.0.1:" + std::to_string(server.bound_port()) + "/api/featuredcategories/?cc=jp&l=japanese";
    src.allow_private_network = true;
    for (const auto& c : fx["trends"]) {
        *current = c["response"];
        const auto topics = fetch_store_trend_topics(c["limit"].get<std::int64_t>(), src);
        CHECK_EQ(json_stringify(topics_json(topics)), json_stringify(c["topics"]));
    }
    // 境界: 応答上限ちょうどは読む・+1 はフォールバック
    const std::string head = R"({"top_sellers":{"items":[{"name":"Big"}]},"pad":")";
    *current = head + std::string(kSteamFeaturedMaxBytes - head.size() - 2, 'x') + "\"}";
    CHECK_EQ(fetch_store_trend_topics(5, src)[0].title, std::string("【話題】Big: 何が支持されている?"));
    *current = head + std::string(kSteamFeaturedMaxBytes - head.size() - 1, 'x') + "\"}";
    CHECK_EQ(fetch_store_trend_topics(5, src).size(), std::size_t{5});
    server.stop();
    // 既定 (allow_private_network=false) では loopback へ出ない (SSRF 対策) → フォールバック
    StoreTrendSource blocked = src;
    blocked.allow_private_network = false;
    CHECK_EQ(fetch_store_trend_topics(2, blocked).size(), std::size_t{2});
}

TEST_CASE("期限: 応答しないサーバはタイムアウトでフォールバック") {
    auto router = std::make_shared<http::Router>();
    router->get("/slow", [](http::Request&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        return http::Response::text("{}");
    });
    http::ServerOptions o;
    o.port = 0;
    http::Server server(o, router);
    server.start();
    StoreTrendSource src;
    src.url = "http://127.0.0.1:" + std::to_string(server.bound_port()) + "/slow";
    src.allow_private_network = true;
    src.timeout = std::chrono::milliseconds(300);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(fetch_steam_top_seller_names(5, src).empty());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1400));
    server.stop();
}

DI_TEST_MAIN()
