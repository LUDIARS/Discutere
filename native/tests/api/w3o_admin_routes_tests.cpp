// w3o_admin_routes のテスト (1/3)。準備は w3o_admin_routes_support.hpp。
#include "w3o_admin_routes_support.hpp"

using namespace w3o_admin_routes_support;

// ── admin 判定 ──────────────────────────────────────────────────────────────

TEST_CASE("admin 判定: noise / datasource / queue は userId 無し 401・role≠admin 403") {
    Env e;
    for (const auto& [router, method, path] : std::vector<std::tuple<http::Router, std::string, std::string>>{
             {mounted(noise_routes()), "GET", "/api/admin/noise"},
             {mounted(noise_routes()), "GET", "/api/admin/noise/groups"},
             {mounted(noise_routes()), "GET", "/api/admin/noise/utterances?sessionId=s"},
             {mounted(noise_routes()), "POST", "/api/admin/noise/exclude"},
             {mounted(noise_routes()), "POST", "/api/admin/noise/restore"},
             {mounted(datasource_routes()), "GET", "/api/admin/datasources"},
             {mounted(datasource_routes()), "GET", "/api/admin/datasources/steam/utterances"},
             {mounted(datasource_routes()), "POST", "/api/admin/datasources/exclude"},
             {mounted(datasource_routes()), "POST", "/api/admin/datasources/active-kg"},
             {mounted(queue_routes()), "GET", "/api/admin/queue"}}) {
        const auto unauth = call(router, method, path, "{}", "", "");
        if (unauth.status != 401) throw test::Failure(path + " unauth " + std::to_string(unauth.status));
        CHECK_EQ(body_of(unauth)["error"].get<std::string>(), std::string("Authentication required"));
        const auto general = call(router, method, path, "{}", "general");
        if (general.status != 403) throw test::Failure(path + " general " + std::to_string(general.status));
        CHECK_EQ(body_of(general)["error"].get<std::string>(), std::string("admin role required"));
    }
}

// ── persona routes (tests/flow/persona-routes.test.ts) ──────────────────────

TEST_CASE("persona-routes.test: autoAdoptOnCrawl の保存・Voluptas 仮名だけ取り込む・NDJSON の壊れた行") {
    Env e(Json{{"flow", Json{{"autoAdoptOnCrawl", false}, {"other", 1}}}, {"keep", "x"}});
    auto app = mounted(persona_routes());
    auto before = call(app, "GET", "/api/admin/personas/data");
    CHECK_EQ(before.status, 200);
    CHECK_EQ(body_of(before)["autoAdoptOnCrawl"].get<bool>(), false);
    CHECK(body_of(before)["envOverride"].is_null());
    CHECK_EQ(body_of(before)["imported"].get<int>(), 0);

    auto saved = call(app, "PUT", "/api/admin/personas/auto-adopt", R"({"enabled":true})");
    CHECK_EQ(saved.status, 200);
    CHECK_EQ(body_of(saved)["autoAdoptOnCrawl"].get<bool>(), true);
    const auto file = parse_json_or_throw(*fs::read_file(e.config_file));
    CHECK_EQ(file["flow"]["autoAdoptOnCrawl"].get<bool>(), true);
    CHECK_EQ(file["flow"]["other"].get<int>(), 1);
    CHECK_EQ(file["keep"].get<std::string>(), std::string("x"));
    CHECK(str::ends_with(*fs::read_file(e.config_file), "}\n"));
    // 文字列の真偽 ("off") と enabled 欠落 (400)
    CHECK_EQ(body_of(call(app, "PUT", "/api/admin/personas/auto-adopt", R"({"enabled":" off "})"))["autoAdoptOnCrawl"].get<bool>(), false);
    CHECK_EQ(body_of(call(app, "PUT", "/api/admin/personas/auto-adopt", R"({"enabled":"YES"})"))["autoAdoptOnCrawl"].get<bool>(), true);
    CHECK_EQ(call(app, "PUT", "/api/admin/personas/auto-adopt", "{}").status, 400);
    CHECK_EQ(call(app, "PUT", "/api/admin/personas/auto-adopt", "壊れた").status, 400);
    CHECK_EQ(call(app, "PUT", "/api/admin/personas/auto-adopt", "null").status, 500);

    Json vec = Json::array();
    for (int i = 0; i < 20; ++i) vec.push_back(0.1);
    auto imported = call(app, "POST", "/api/admin/personas/import",
                         json_stringify(Json{{"personas", Json::array({Json{{"user_id", "ext:voluptas:abcdef0123456789"},
                                                                             {"affect_vector", vec},
                                                                             {"vector_spec_version", 1},
                                                                             {"traits", Json::array({"協力型"})}}})}}));
    CHECK_EQ(imported.status, 200);
    CHECK_EQ(body_of(imported)["ok"].get<bool>(), true);
    CHECK_EQ(body_of(imported)["inserted"].get<int>(), 1);
    auto raw_sid = call(app, "POST", "/api/admin/personas/import",
                        json_stringify(Json::array({Json{{"user_id", "raw-sid"}, {"affect_vector", vec}, {"vector_spec_version", 1}}})));
    CHECK_EQ(body_of(raw_sid)["skipped"].get<int>(), 1);

    Json vec2 = Json::array();
    for (int i = 0; i < 20; ++i) vec2.push_back(0.2);
    const std::string ndjson =
        json_stringify(Json{{"pseudoId", "ext:voluptas:fedcba9876543210"}, {"affectVector", vec2}, {"vectorSpecVersion", 1}}) + "\nnot-json\n";
    auto nd = call(app, "POST", "/api/admin/personas/import", ndjson, "admin", "u1", "application/x-ndjson");
    CHECK_EQ(nd.status, 200);
    CHECK_EQ(body_of(nd)["inserted"].get<int>(), 1);
    CHECK_EQ(body_of(nd)["skipped"].get<int>(), 1);
    W3O_SAME(body_of(nd)["skipReasons"], (Json{{"invalid-json", 1}}));
    CHECK_EQ(body_of(call(app, "GET", "/api/admin/personas/data"))["imported"].get<int>(), 2);
}

TEST_CASE("persona routes: 画面・adopt の既定値と範囲 (minOpinions 1..10000, dry 既定 true)") {
    Env e;
    Json last;
    PersonaRouteDeps deps;
    deps.runAdopt = [&last](const flow::AdoptRunOptions& o) {
        last = Json{{"source", o.sourceFilter ? Json(*o.sourceFilter) : Json(nullptr)}, {"min", *o.minOpinions}, {"dry", o.dry}};
        flow::AdoptRunSummary s;
        s.speakers = 3;
        s.rejectedByReason = {{"too-few", 2}};
        return s;
    };
    auto app = mounted(persona_routes(deps));
    const auto page = call(app, "GET", "/api/admin/personas");
    CHECK_EQ(page.status, 200);
    CHECK(str::contains(page.body, "匿名ペルソナ管理"));
    auto r = call(app, "POST", "/api/admin/personas/adopt", "{}");
    CHECK_EQ(r.status, 200);
    W3O_SAME(body_of(r), (Json{{"ok", true}, {"dry", true}, {"sourceFilter", nullptr}, {"minOpinions", 10}, {"speakers", 3}, {"utterances", 0},
                               {"adopted", 0}, {"rejected", 0}, {"rejectedByReason", Json{{"too-few", 2}}}}));
    call(app, "POST", "/api/admin/personas/adopt", R"({"source":"  youtube ","minOpinions":"99999","dry":false})");
    W3O_SAME(last, (Json{{"source", "youtube"}, {"min", 10000}, {"dry", false}}));
    call(app, "POST", "/api/admin/personas/adopt", R"({"source":"  ","minOpinions":-5.5,"dry":"on"})");
    W3O_SAME(last, (Json{{"source", nullptr}, {"min", 1}, {"dry", true}}));
    call(app, "POST", "/api/admin/personas/adopt", R"({"minOpinions":"abc"})");
    CHECK_EQ(last["min"].get<int>(), 10);
    CHECK_EQ(call(app, "POST", "/api/admin/personas/adopt", "null").status, 500);
    PersonaRouteDeps failing;
    failing.runAdopt = [](const flow::AdoptRunOptions&) -> flow::AdoptRunSummary { throw std::runtime_error("kg missing"); };
    auto bad = mounted(persona_routes(failing));
    const auto f = call(bad, "POST", "/api/admin/personas/adopt", "{}");
    CHECK_EQ(f.status, 500);
    CHECK_EQ(body_of(f)["error"].get<std::string>(), std::string("kg missing"));
}

TEST_CASE("境界: persona import の本文上限 (16MiB ちょうどは読む / +1 は 413) と巨大な壊れた本文") {
    Env e;
    auto app = mounted(persona_routes());
    CHECK_EQ(app.max_body_for("POST", "/api/admin/personas/import"), kPersonaImportMaxBody);
    // 上限ちょうどの空白 → 空文書 (0 件)
    auto exact = call(app, "POST", "/api/admin/personas/import", std::string(kPersonaImportMaxBody, ' '));
    CHECK_EQ(exact.status, 200);
    CHECK_EQ(body_of(exact)["imported"].get<int>(), 0);
    // 壊れた行だらけ (巨大) でも 200 で skip 件数だけ返す
    std::string junk;
    for (int i = 0; i < 1000; ++i) junk += "{broken\n";
    auto broken = call(app, "POST", "/api/admin/personas/import", junk);
    CHECK_EQ(broken.status, 200);
    CHECK_EQ(body_of(broken)["skipped"].get<int>(), 1000);
}

DI_TEST_MAIN()
