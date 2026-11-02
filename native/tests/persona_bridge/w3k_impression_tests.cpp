// w3-K: Voluptas → Discutere の感想受け取り (tests/flow/impression-bridge.test.ts の検査・取込・受け口) と境界。
#include <memory>

#include "check.hpp"
#include "core/test_util.hpp"

#include "api/impression_bridge_routes.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "flow/user_voices/collect.hpp"
#include "infra/http_client.hpp"
#include "infra/http_server.hpp"
#include "persona_bridge/impression_intake.hpp"

using namespace di;
using namespace di::persona_bridge;

namespace {

Json good() {
    return Json{{"id", "imp-1"},        {"gameTitle", "テストゲーム"}, {"recommend", true},
                {"polarity", "positive"}, {"comment", "周回が楽しい"}, {"createdAt", "2026-10-01T00:00:00.000Z"}};
}

Json with(Json base, const std::string& key, Json value) {
    base[key] = std::move(value);
    return base;
}

const std::string TOKEN(40, 't');

struct Fixture {
    std::vector<std::string> ingested;
    bool fail = false;
    http::Router router;
    explicit Fixture(std::string token = TOKEN) {
        api::ImpressionBridgeRoutesOptions o;
        o.token = [token] { return token; };
        o.serviceAudience = [] { return std::string("discutere"); };
        o.loadServiceTokenKeys = [] { return std::vector<cernere_service_token::Ed25519PublicKey>{}; };
        o.withIngest = [this](const std::function<void(const IngestDeps&)>& run) {
            if (fail) throw std::runtime_error("/secret/kg/path.kuzu: disk I/O error");
            IngestDeps d;
            d.importItems = [this](const std::vector<crawler::sources::ExternalUtterance>& items) {
                for (const auto& i : items) ingested.push_back(i.nativeId);
                return static_cast<int>(items.size());
            };
            run(d);
        };
        router.mount("/api", api::create_impression_bridge_routes(o));
    }
    http::Response post(const std::string& body, std::optional<std::string> auth = std::nullopt) {
        http::Request req;
        req.method = "POST";
        req.target = req.path = "/api/persona-bridge/impressions";
        req.headers.set("content-type", "application/json");
        if (auth) req.headers.set("authorization", *auth);
        req.body = body;
        req.remote_address = "203.0.113.5";
        return router.handle(req);
    }
};

Json body_of(const http::Response& r) { return parse_json_or_throw(r.body, {8u << 20, 1'000'000, 64}); }

} // namespace

TEST_CASE("golden: parse_impression_batch (匿名性・形・件数・境界)") {
    for (const auto& c : test::load_fixture("w3k/impression_batches.json")) {
        const Json& body = c["body"];
        if (c["ok"].get<bool>()) {
            const auto r = parse_impression_batch(body);
            Json list = Json::array();
            for (const auto& i : r.impressions) {
                Json j{{"id", i.id}, {"gameTitle", i.gameTitle}};
                j["recommend"] = i.recommend ? Json(*i.recommend) : Json(nullptr);
                j["polarity"] = i.polarity ? Json(*i.polarity) : Json(nullptr);
                j["comment"] = i.comment;
                j["createdAt"] = i.createdAt;
                list.push_back(j);
            }
            const Json actual{{"impressions", list}, {"skipped", to_json(r.skipped)}};
            CHECK_EQ(json_stringify(actual), json_stringify(c["value"]));
        } else {
            bool threw = false;
            try {
                parse_impression_batch(body);
            } catch (const ImpressionBatchError& e) {
                threw = true;
                CHECK_EQ(std::string(e.what()), c["error"].get<std::string>());
            }
            CHECK(threw);
        }
    }
}

TEST_CASE("parse_impression_batch: 書き手の項目は拒否・壊れた形は invalid・件数上限") {
    const auto r = parse_impression_batch(Json{{"impressions", Json::array({good(), with(good(), "userId", "u-123"), with(good(), "authorName", "山田"),
                                                                            with(good(), "comment", ""), with(good(), "createdAt", "not-a-date"), "文字列",
                                                                            Json{{"id", "imp-6"}, {"gameTitle", "テストゲーム"}, {"comment", "最低限"},
                                                                                 {"createdAt", "2026-10-02T00:00:00Z"}}})}});
    CHECK(r.impressions.size() == 2 && r.impressions[0].id == "imp-1" && r.impressions[1].id == "imp-6");
    CHECK_EQ(r.skipped.non_anonymous_field, 2);
    CHECK_EQ(r.skipped.invalid, 3);
    CHECK(!r.impressions[1].recommend);
    CHECK_THROWS(parse_impression_batch(Json::object()));
    Json many = Json::array();
    for (std::size_t i = 0; i <= IMPRESSION_PUSH_MAX; ++i) many.push_back(good());
    CHECK_THROWS(parse_impression_batch(Json{{"impressions", many}}));
    many.erase(many.size() - 1);
    CHECK_EQ(parse_impression_batch(Json{{"impressions", many}}).impressions.size(), IMPRESSION_PUSH_MAX);
    // 入れ子の値を持つ書き手の項目も 1 件ごと拒否
    CHECK_EQ(parse_impression_batch(Json{{"impressions", Json::array({with(good(), "author", Json{{"name", "x"}})})}}).skipped.non_anonymous_field, 1);
}

TEST_CASE("ingest_impressions: 書き手は常に匿名・ゲーム単位 slug・埋め込み失敗は止めない") {
    std::vector<crawler::sources::ExternalUtterance> imported;
    auto p = parse_impression_batch(Json{{"impressions", Json::array({good(), with(with(good(), "id", "imp-9"), "gameTitle", "別のゲーム")})}});
    IngestDeps d;
    d.importItems = [&](const std::vector<crawler::sources::ExternalUtterance>& items) {
        for (const auto& i : items) imported.push_back(i);
        return static_cast<int>(items.size());
    };
    std::vector<std::string> ids;
    d.embed = [&](const std::vector<std::string>& v) {
        ids = v;
        return static_cast<int>(v.size());
    };
    const auto r = ingest_impressions(p.impressions, d);
    CHECK(r.received == 2 && r.imported == 2 && r.embedded == 2);
    for (const auto& u : imported) CHECK(u.source == "glab" && u.authorId == "anonymous" && !u.authorName);
    CHECK_EQ(imported[0].gameSlug, flow::user_voices::glab_game_slug("テストゲーム"));
    CHECK_EQ(r.games[flow::user_voices::glab_game_slug("別のゲーム")].get<int>(), 1);
    CHECK((ids == std::vector<std::string>{"ext:glab:imp-1", "ext:glab:imp-9"}));

    IngestDeps failing;
    failing.importItems = [](const std::vector<crawler::sources::ExternalUtterance>&) { return 1; };
    failing.embed = [](const std::vector<std::string>&) -> int { throw std::runtime_error("down"); };
    std::vector<std::string> warns;
    failing.warn = [&](const std::string& m) { warns.push_back(m); };
    CHECK_EQ(ingest_impressions(p.impressions, failing).embedded, 0);
    CHECK(warns.size() == 1);
    IngestDeps none;
    none.importItems = [](const std::vector<crawler::sources::ExternalUtterance>&) { return 99; };
    const auto empty = ingest_impressions({}, none);
    CHECK(empty.imported == 0 && empty.received == 0);
}

TEST_CASE("POST /persona-bridge/impressions: 認証 (無し 401 / 不一致 401 / 一致 200) と本文エラー 400、no-store") {
    Fixture f;
    const std::string one = json_stringify(Json{{"impressions", Json::array({good()})}});
    const auto no_auth = f.post(one);
    CHECK_EQ(no_auth.status, 401);
    CHECK_EQ(no_auth.headers.get("cache-control").value_or(""), std::string("private, no-store"));
    CHECK_EQ(f.post(one, "Bearer wrong").status, 401);
    CHECK_EQ(f.post(one, "Bearer " + TOKEN.substr(1) + "x").status, 401);
    CHECK_EQ(f.post(one, "bearer " + TOKEN).status, 401);
    CHECK(f.ingested.empty());

    const auto ok = f.post(json_stringify(Json{{"impressions", Json::array({good(), with(with(good(), "id", "x"), "userId", "u")})}}), "Bearer " + TOKEN);
    CHECK_EQ(ok.status, 200);
    CHECK_EQ(ok.headers.get("cache-control").value_or(""), std::string("private, no-store"));
    const Json b = body_of(ok);
    CHECK_EQ(json_stringify(b), json_stringify(Json{{"ok", true},
                                                    {"received", 1},
                                                    {"imported", 1},
                                                    {"embedded", 0},
                                                    {"games", Json{{flow::user_voices::glab_game_slug("テストゲーム"), 1}}},
                                                    {"skipped", Json{{"non_anonymous_field", 1}, {"invalid", 0}}}}));
    CHECK((f.ingested == std::vector<std::string>{"imp-1"}));

    const auto bad = f.post("not json", "Bearer " + TOKEN);
    CHECK_EQ(bad.status, 400);
    CHECK_EQ(body_of(bad)["error"].get<std::string>(), std::string("body must be JSON"));
    const auto nope = f.post(R"({"nope":1})", "Bearer " + TOKEN);
    CHECK_EQ(nope.status, 400);
    CHECK_EQ(body_of(nope)["error"].get<std::string>(), std::string("impressions must be an array"));
    CHECK_EQ(f.post("", "Bearer " + TOKEN).status, 400);
    // 空の配列は 200 (0 件)
    const auto zero = f.post(R"({"impressions":[]})", "Bearer " + TOKEN);
    CHECK_EQ(zero.status, 200);
    CHECK_EQ(body_of(zero)["received"].get<int>(), 0);
}

TEST_CASE("POST /persona-bridge/impressions: 短い固定トークンは未設定扱い (503)・31/32 文字の境界") {
    Fixture short_tok("short");
    CHECK_EQ(short_tok.post(R"({"impressions":[]})", "Bearer short").status, 503);
    Fixture t31(std::string(31, 'k'));
    CHECK_EQ(t31.post(R"({"impressions":[]})", "Bearer " + std::string(31, 'k')).status, 503);
    Fixture t32(std::string(32, 'k'));
    CHECK_EQ(t32.post(R"({"impressions":[]})", "Bearer " + std::string(32, 'k')).status, 200);
    Fixture unset("");
    CHECK_EQ(unset.post(R"({"impressions":[]})", "Bearer x").status, 503);
}

TEST_CASE("POST /persona-bridge/impressions: 取込の内部例外は 500 で中身を出さない") {
    Fixture f;
    f.fail = true;
    std::vector<std::string> logs;
    const int sink = log::add_sink([&](const log::Record& r) { logs.push_back(std::string(r.message)); });
    const auto r = f.post(json_stringify(Json{{"impressions", Json::array({good()})}}), "Bearer " + TOKEN);
    log::remove_sink(sink);
    CHECK_EQ(r.status, 500);
    CHECK_EQ(r.body, std::string(R"({"ok":false,"error":"failed to ingest impressions"})"));
    for (const auto& l : logs) {
        CHECK(!str::contains(l, "/secret/kg"));
        CHECK(!str::contains(l, TOKEN));
    }
}

TEST_CASE("POST /persona-bridge/impressions: JSON の上限 (要素数・深さ・件数) と本文上限 (ちょうど / +1 / 巨大) を実サーバで") {
    Fixture f;
    // 要素数の上限を超える本文は「JSON として読めない」= 400
    Json many = Json::array();
    for (int i = 0; i < 20'000; ++i) many.push_back(i);
    CHECK_EQ(f.post(json_stringify(Json{{"impressions", many}}), "Bearer " + TOKEN).status, 400);
    // 深い入れ子
    CHECK_EQ(f.post(std::string(10'000, '[') + std::string(10'000, ']'), "Bearer " + TOKEN).status, 400);
    // 201 件は件数上限の 400
    Json too = Json::array();
    for (int i = 0; i < 201; ++i) too.push_back(with(good(), "id", "i" + std::to_string(i)));
    const auto r201 = f.post(json_stringify(Json{{"impressions", too}}), "Bearer " + TOKEN);
    CHECK_EQ(r201.status, 400);
    CHECK_EQ(body_of(r201)["error"].get<std::string>(), std::string("impressions must be at most 200 items"));
    // 上限ぎりぎりの正当な本文 (200 件 × 1 万字) は通る
    Json full = Json::array();
    for (int i = 0; i < 200; ++i) full.push_back(with(with(good(), "id", "f" + std::to_string(i)), "comment", str::repeat("あ", 10'000)));
    const std::string full_body = json_stringify(Json{{"impressions", full}});
    CHECK(full_body.size() < kImpressionBodyMaxBytes);
    CHECK_EQ(f.post(full_body, "Bearer " + TOKEN).status, 200);

    auto router = std::make_shared<http::Router>(f.router);
    CHECK_EQ(router->max_body_for("POST", "/api/persona-bridge/impressions"), kImpressionBodyMaxBytes);
    http::ServerOptions o;
    o.port = 0;
    http::Server server(o, router);
    server.start();
    auto send = [&](std::size_t n) {
        http::ClientRequest q;
        q.method = "POST";
        q.url = "http://127.0.0.1:" + std::to_string(server.bound_port()) + "/api/persona-bridge/impressions";
        q.headers.set("Authorization", "Bearer " + TOKEN);
        std::string body = R"({"impressions":[]})";
        body += std::string(n - body.size(), ' ');
        q.body = body;
        q.allow_private_network = true;
        q.max_response_bytes = 1u << 20;
        return http::fetch(q).status;
    };
    CHECK_EQ(send(kImpressionBodyMaxBytes), 200);
    CHECK_EQ(send(kImpressionBodyMaxBytes + 1), 413);
    const int huge = send(64u << 20);
    CHECK(huge == 413 || huge == 0);
    server.stop();
}

DI_TEST_MAIN()
