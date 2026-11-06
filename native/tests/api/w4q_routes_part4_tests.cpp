// w4q_routes のテスト (4/4)。準備は w4q_routes_support.hpp。
#include "w4q_routes_support.hpp"

using namespace w4q_routes_support;

TEST_CASE("blackbox: 全ルートで 401 / 403") {
    BlackboxFixture f;
    http::Router bare;
    bare.mount("/api", api::blackbox_routes([b = f.box] { return b; }));
    const std::vector<std::pair<std::string, std::string>> routes{{"GET", "/api/admin/blackbox/decisions"},
                                                                  {"POST", "/api/admin/blackbox/decisions/1/verdict"},
                                                                  {"GET", "/api/admin/blackbox/rules"},
                                                                  {"POST", "/api/admin/blackbox/rules/r1/state"}};
    for (const auto& [m, t] : routes) {
        CHECK_EQ(call(bare, m, t).status, 401);
        CHECK_EQ(call(f.app, m, t, Json{{"X-User-Id", "u"}}, R"({"verdict":"ok","state":"auto"})").status, 403);
        CHECK_EQ(call(f.app, m, t, Json::object(), R"({"verdict":"ok","state":"auto"})", "10.0.0.1").status, 403);
    }
}

TEST_CASE("blackbox: decisions の一覧と verdict の検証 (id / verdict / 本文)") {
    BlackboxFixture f;
    lib::blackbox::DecisionInput d;
    d.domain = flow::DOMAIN_DENSITY;
    d.input = Json::object();
    d.output = "rich";
    d.source = "llm";
    d.status = "pending_review";
    d.createdAt = "2026-01-01T00:00:00.000Z";
    const auto id = f.box.ledger->record(d);

    auto list = body_json(f.admin("GET", "/api/admin/blackbox/decisions?limit=abc"));
    CHECK_EQ(list["items"].size(), std::size_t{1});
    CHECK_EQ(body_json(f.admin("GET", "/api/admin/blackbox/decisions?domain=other"))["items"].size(), std::size_t{0});
    CHECK_EQ(body_json(f.admin("GET", "/api/admin/blackbox/decisions?domain=&limit=-5"))["items"].size(), std::size_t{1});

    CHECK_EQ(f.admin("POST", "/api/admin/blackbox/decisions/abc/verdict", R"({"verdict":"ok"})").status, 400);
    CHECK_EQ(f.admin("POST", "/api/admin/blackbox/decisions/1.5/verdict", R"({"verdict":"ok"})").status, 400);
    auto r = f.admin("POST", "/api/admin/blackbox/decisions/" + std::to_string(id) + "/verdict", R"({"verdict":"OK"})");
    CHECK_EQ(r.status, 400);
    CHECK_EQ(body_json(r)["error"].get<std::string>(), std::string("verdict must be 'ok' or 'ng'"));
    CHECK_EQ(f.admin("POST", "/api/admin/blackbox/decisions/" + std::to_string(id) + "/verdict", "{broken").status, 400);
    CHECK_EQ(f.admin("POST", "/api/admin/blackbox/decisions/" + std::to_string(id) + "/verdict", "null").status, 500);
    CHECK_EQ(f.admin("POST", "/api/admin/blackbox/decisions/999/verdict", R"({"verdict":"ok"})").status, 404);
    CHECK_EQ(f.admin("POST", "/api/admin/blackbox/decisions/1e300/verdict", R"({"verdict":"ok"})").status, 404);
    // 巨大な本文は JSON の上限で読まない → 400
    CHECK_EQ(f.admin("POST", "/api/admin/blackbox/decisions/1/verdict", R"({"verdict":")" + std::string(70'000, 'o') + R"("})").status, 400);

    r = f.admin("POST", "/api/admin/blackbox/decisions/" + std::to_string(id) + "/verdict", R"({"verdict":"ok"})");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(json_stringify(body_json(r)), std::string(R"({"ok":true,"rule":null})"));
    CHECK_EQ(body_json(f.admin("GET", "/api/admin/blackbox/decisions"))["items"].size(), std::size_t{0});
}

TEST_CASE("blackbox: rules の一覧 (whenText 付き) と state の変更") {
    BlackboxFixture f;
    lib::blackbox::RuleDraft draft;
    draft.domain = flow::DOMAIN_DENSITY;
    draft.description = "声が多ければ rich";
    draft.when = Json{{"op", "cmp"}, {"feature", "voiceCount"}, {"cmp", ">="}, {"value", 10}};
    draft.output = "rich";
    const auto rule = f.box.engine->add_rule(draft);

    auto body = body_json(f.admin("GET", "/api/admin/blackbox/rules"));
    CHECK_EQ(body["rules"].size(), std::size_t{1});
    CHECK(body["rules"][0].contains("whenText"));
    CHECK_EQ(body["rules"][0]["whenText"].get<std::string>(), lib::blackbox::describe_condition(draft.when));
    CHECK_EQ(body["stats"].size(), std::size_t{1});
    CHECK_EQ(body_json(f.admin("GET", "/api/admin/blackbox/rules?domain=other"))["rules"].size(), std::size_t{0});

    auto r = f.admin("POST", "/api/admin/blackbox/rules/" + rule.id + "/state", R"({"state":"bogus"})");
    CHECK_EQ(r.status, 400);
    CHECK_EQ(body_json(r)["error"].get<std::string>(), std::string("state must be one of candidate,trial,auto,retired"));
    CHECK_EQ(f.admin("POST", "/api/admin/blackbox/rules/" + rule.id + "/state", R"({"state":1})").status, 400);
    CHECK_EQ(f.admin("POST", "/api/admin/blackbox/rules/none/state", R"({"state":"auto"})").status, 404);
    r = f.admin("POST", "/api/admin/blackbox/rules/" + rule.id + "/state", R"({"state":"retired"})");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(body_json(r)["rule"]["state"].get<std::string>(), std::string("retired"));
}

DI_TEST_MAIN()
