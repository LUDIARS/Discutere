// flow/user-voices/steam-app (tests/flow/user-voices.test.ts の Steam 部分 + golden fixtures/w1f/steam.json)。
#include "check.hpp"
#include "w1f_support.hpp"

#include "base/strings.hpp"
#include "flow/user_voices/nfkc.hpp"
#include "flow/user_voices/steam_app.hpp"

using namespace di;
using namespace di::flow::user_voices;
using namespace di::test::w1f;

namespace {

http::ClientResponse json_response(const Json& body, int status = 200) {
    http::ClientResponse r;
    r.transport_ok = true;
    r.status = status;
    r.body = json_stringify(body);
    return r;
}

} // namespace

TEST_CASE("normalize_game_name / is_same_game match Node (NFKC + toLowerCase approximations)") {
    const Json fx = load_fixture("steam.json");
    for (const auto& c : fx["normalized"])
        if (normalize_game_name(c["input"].get<std::string>()) != c["output"].get<std::string>())
            throw test::Failure("normalize " + c["input"].get<std::string>() + " -> " + normalize_game_name(c["input"].get<std::string>()));
    for (const auto& c : fx["same"])
        CHECK_EQ(is_same_game(c["candidate"].get<std::string>(), c["query"].get<std::string>()), c["same"].get<bool>());
}

TEST_CASE("resolve_steam_app: adopts the candidate contained in the theme and judges release") {
    std::vector<std::string> urls;
    auto fake = [&](bool released) {
        return [&urls, released](const http::ClientRequest& req) {
            urls.push_back(req.url);
            CHECK_EQ(req.headers.get("User-Agent").value(), "LUDIARS-Discutere-Crawler/0.1 (+https://github.com/LUDIARS)");
            CHECK(req.max_response_bytes <= kSteamMaxResponseBytes);
            if (str::contains(req.url, "storesearch"))
                return json_response(Json::parse(R"({"items":[{"id":1,"name":"パズル&ドラゴンズ"},{"id":2,"name":"モンスターストライク"}]})"));
            Json details = Json::object();
            details["2"] = Json{{"success", true},
                                {"data", Json{{"name", "モンスターストライク"}, {"release_date", Json{{"coming_soon", !released}}}}}};
            return json_response(details);
        };
    };
    ResolveSteamAppArgs args;
    args.terms = {"モンスト", "モンスターストライク"};
    args.matchAgainst = "モンスターストライクのガチャ天井を下げるか";
    args.fetchImpl = fake(true);
    const auto app = resolve_steam_app(args);
    CHECK(app.has_value());
    CHECK(*app == (SteamApp{2, "モンスターストライク", true}));
    CHECK(str::contains(urls[0], "storesearch") && str::contains(urls[1], "appids=2"));
    CHECK_EQ(urls[0], "https://store.steampowered.com/api/storesearch/?term=%E3%83%A2%E3%83%B3%E3%82%B9%E3%83%88&l=japanese&cc=JP");
    CHECK_EQ(json_stringify(to_json(*app)), R"({"appId":2,"name":"モンスターストライク","released":true})");

    ResolveSteamAppArgs b;
    b.terms = {"x"};
    b.matchAgainst = "モンスターストライク";
    b.fetchImpl = fake(false);
    CHECK_EQ(resolve_steam_app(b)->released, false);

    int calls = 0;
    ResolveSteamAppArgs c;
    c.terms = {"a", "b", "c", "d", " a ", ""};
    c.matchAgainst = "全然違うゲーム";
    c.maxQueries = 2;
    c.fetchImpl = [&](const http::ClientRequest&) {
        ++calls;
        return json_response(Json::parse(R"({"items":[{"id":9,"name":"無関係"}]})"));
    };
    CHECK(!resolve_steam_app(c).has_value());
    CHECK_EQ(calls, 2);  // maxQueries で打ち切る
}

TEST_CASE("resolve_steam_app: failures (non-2xx = none, transport / broken JSON = throw, odd shapes skipped)") {
    ResolveSteamAppArgs a;
    a.terms = {"モンスターストライク"};
    a.matchAgainst = "モンスターストライク";
    a.fetchImpl = [](const http::ClientRequest&) { return json_response(Json::object(), 503); };
    CHECK(!resolve_steam_app(a).has_value());
    a.fetchImpl = [](const http::ClientRequest&) {
        http::ClientResponse r;
        r.error = "response too large";
        return r;
    };
    CHECK_THROWS(resolve_steam_app(a));
    a.fetchImpl = [](const http::ClientRequest&) {
        http::ClientResponse r;
        r.transport_ok = true;
        r.status = 200;
        r.body = "{not json";
        return r;
    };
    CHECK_THROWS(resolve_steam_app(a));
    // 深すぎる JSON (上限超過) も壊れた応答として扱う
    a.fetchImpl = [](const http::ClientRequest&) {
        http::ClientResponse r;
        r.transport_ok = true;
        r.status = 200;
        r.body = str::repeat("[", 1000) + str::repeat("]", 1000);
        return r;
    };
    CHECK_THROWS(resolve_steam_app(a));
    // items が配列でない / id が文字列 / success が false → 見つからない
    for (const char* search : {R"({"items":"x"})", R"({"items":[{"id":"2","name":"モンスターストライク"}]})", R"([1,2])",
                               R"({"items":[{"id":2.5,"name":"モンスターストライク"}]})"}) {
        a.fetchImpl = [search](const http::ClientRequest&) { return json_response(Json::parse(search)); };
        CHECK(!resolve_steam_app(a).has_value());
    }
    a.fetchImpl = [](const http::ClientRequest& req) {
        if (str::contains(req.url, "storesearch")) return json_response(Json::parse(R"({"items":[{"id":2,"name":"モンスターストライク"}]})"));
        return json_response(Json::parse(R"({"2":{"success":false}})"));
    };
    CHECK(!resolve_steam_app(a).has_value());
    // appdetails の name が無ければ検索結果の名前、release_date が無ければ未リリース
    a.fetchImpl = [](const http::ClientRequest& req) {
        if (str::contains(req.url, "storesearch")) return json_response(Json::parse(R"({"items":[{"id":2,"name":"モンスターストライク"}]})"));
        return json_response(Json::parse(R"({"2":{"success":1,"data":{}}})"));
    };
    const auto app = resolve_steam_app(a);
    CHECK(app && app->name == "モンスターストライク" && !app->released);
}

TEST_CASE("nfkc helpers") {
    CHECK_EQ(nfkc_approx("ﾊﾟ"), "パ");
    CHECK_EQ(nfkc_approx("が"), "が");
    CHECK_EQ(nfkc_approx("①"), "1");
    CHECK(compose_pair('e', 0x301).has_value());
    CHECK(decompose_pair(0x304C).has_value());
    CHECK_EQ(js_to_lower("ΣΑΣ"), "σας");
    // 巨大入力でも線形
    CHECK_EQ(normalize_game_name(str::repeat("Ａ ", 200000)).size(), 200000u);
}

DI_TEST_MAIN()
