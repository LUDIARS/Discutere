// w3-O: admin 系ルート (persona / noise / datasource / queue / kg-migration) のテスト。
// 移した TS テスト: tests/flow/persona-routes.test.ts。それ以外は TS にテストが無いので、TS の挙動
// (admin 判定 401/403・必須項目 400・応答の形・config の書き戻し) を確かめる。
// 境界: 本文上限 (import 16MiB ちょうど/+1)・壊れた本文・null 本文・巨大なクエリ・kg token 比較。
// DB / config は一時ディレクトリ (本物の data/・discutere.config.json は読まない)。
#include "w3o/w3o_support.hpp"

#include <filesystem>

#include "api/datasource_routes.hpp"
#include "api/kg_migration_routes.hpp"
#include "api/noise_routes.hpp"
#include "api/persona_routes.hpp"
#include "api/queue_routes.hpp"
#include "base/env.hpp"
#include "base/strings.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/noise/exclusions.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "crawler/sources/ingested_store.hpp"
#include "flow/db/connection.hpp"

using namespace di;
using namespace di::api;
using w3o::TempDir;

namespace {

/** 一時ディレクトリに config / KG / flow DB を置き、作業ディレクトリもそこへ移す。 */
struct Env {
    TempDir dir{"w3o-admin-"};
    std::filesystem::path old_cwd = std::filesystem::current_path();
    std::string config_file;
    std::string kg_path;
    explicit Env(const Json& config = Json::object()) {
        std::filesystem::current_path(dir.path);
        config_file = dir.path + "/discutere.config.json";
        kg_path = dir.path + "/kg/discatier.sqlite";
        fs::ensure_dir(dir.path + "/kg");
        CHECK(fs::write_file_atomic(config_file, json_stringify_pretty(config, 2) + "\n"));
        env::set_for_test("DISCUTERE_CONFIG", std::string_view(config_file));
        env::set_for_test("DATABASE_PATH", std::string_view(dir.path + "/discutere.db"));
        env::set_for_test("DISCATIER_KUZU_PATH", std::string_view(kg_path));
        env::set_for_test("DISCUTERE_PERSONA_ENGINE_DB", std::string_view(dir.path + "/persona-engine.db"));
        env::set_for_test("DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL", std::nullopt);
        env::set_for_test("DISCATIER_WORKSPACE", std::nullopt);
        config::reset_config_for_test();
        flow::db::reset_flow_db_for_test();
    }
    ~Env() {
        flow::db::reset_flow_db_for_test();
        config::reset_config_for_test();
        std::error_code ec;
        std::filesystem::current_path(old_cwd, ec);
    }
    std::unique_ptr<core::Core> core() const { return core::create_core(kg_path); }
};

http::Response call(const http::Router& r, const std::string& method, const std::string& path, const std::string& body = "",
                    const std::string& role = "admin", const std::string& user = "u1", const std::string& content_type = "application/json") {
    http::Request req;
    req.method = method;
    req.target = path;
    const auto q = path.find('?');
    req.path = path.substr(0, q);
    if (q != std::string::npos) req.query_string = path.substr(q + 1);
    req.remote_address = "127.0.0.1";
    if (!user.empty()) req.headers.set("X-User-Id", user);
    if (!role.empty()) req.headers.set("X-User-Role", role);
    req.headers.set("content-type", content_type);
    req.body = body;
    return r.handle(req);
}

Json body_of(const http::Response& r) { return parse_json_or_throw(r.body); }

http::Router mounted(const http::Router& sub) {
    http::Router app;
    app.mount("/api", sub);
    return app;
}

void insert_session(core::Core& c, const std::string& id, const std::string& title, const std::string& ws = "knowledge") {
    c.raw().run("INSERT INTO sessions (id, workspace_id, title, started_at, created_at, updated_at) VALUES (?, ?, ?, 1, 1, 1)", {id, ws, title});
}
void insert_utterance(core::Core& c, const std::string& id, const std::string& session, const std::optional<std::string>& speaker,
                      const std::string& content, std::int64_t at) {
    c.raw().run(
        "INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES (?, 'knowledge', ?, ?, ?, ?, 1, 1)",
        {id, session, speaker, content, at});
}

} // namespace

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

// ── noise routes ────────────────────────────────────────────────────────────

TEST_CASE("noise routes: groups / utterances / exclude / restore") {
    Env e;
    {
        auto c = e.core();
        c->raw().exec("CREATE TABLE IF NOT EXISTS personas (id TEXT PRIMARY KEY, display_name TEXT)");
        c->raw().run("INSERT INTO personas (id, display_name) VALUES ('p1', 'ペルソナ壱')");
        c->raw().run("INSERT INTO design_gaps (id, workspace_id, title, created_at, updated_at) VALUES ('g1', 'knowledge', '議題1', 1, 1)");
        insert_session(*c, "s1", "discussion-of-gap:g1");
        insert_session(*c, "s2", "discussion-of-gap:g-missing");
        insert_session(*c, "s3", "steam:alpha");
        insert_utterance(*c, "u1", "s1", std::string("persona:p1"), "発話1", 10);
        insert_utterance(*c, "u2", "s1", std::string("ext:steam:9"), "外部", 20);
        insert_utterance(*c, "u3", "s1", std::string("123"), "人間", 30);
        insert_utterance(*c, "u4", "s1", std::nullopt, "進行", 40);
        insert_utterance(*c, "u5", "s3", std::string("persona:zz"), "未登録", 50);
        c->close();
    }
    auto app = mounted(noise_routes());
    CHECK(str::contains(call(app, "GET", "/api/admin/noise").body, "ノイズ管理"));
    const auto groups = body_of(call(app, "GET", "/api/admin/noise/groups"));
    CHECK_EQ(groups["sessions"].size(), 2u);
    W3O_SAME(groups["sessions"][0], (Json{{"sessionId", "s1"}, {"gapId", "g1"}, {"title", "議題1"}, {"utteranceCount", 4}}));
    CHECK_EQ(groups["sessions"][1]["title"].get<std::string>(), std::string("discussion-of-gap:g-missing"));
    W3O_SAME(groups["sources"][0], (Json{{"source", "discussion-of-gap"}, {"count", 2}}));

    CHECK_EQ(call(app, "GET", "/api/admin/noise/utterances").status, 400);
    CHECK_EQ(call(app, "GET", "/api/admin/noise/utterances?sessionId=").status, 400);
    CHECK_EQ(call(app, "POST", "/api/admin/noise/exclude", R"({"reason":"x"})").status, 400);
    CHECK_EQ(call(app, "POST", "/api/admin/noise/exclude", "壊れた").status, 400);
    CHECK_EQ(call(app, "POST", "/api/admin/noise/exclude", "null").status, 500);
    CHECK_EQ(call(app, "POST", "/api/admin/noise/exclude", R"({"utteranceId":"u2","reason":"煽り"})").status, 200);
    auto list = body_of(call(app, "GET", "/api/admin/noise/utterances?sessionId=s1"));
    CHECK_EQ(list.size(), 4u);
    W3O_SAME(list[0], (Json{{"id", "u1"}, {"speaker", "ペルソナ壱"}, {"content", "発話1"}, {"posted_at", 10}, {"excluded", false}}));
    CHECK_EQ(list[1]["speaker"].get<std::string>(), std::string("外部の声"));
    CHECK_EQ(list[1]["excluded"].get<bool>(), true);
    CHECK_EQ(list[2]["speaker"].get<std::string>(), std::string("人間"));
    CHECK_EQ(list[3]["speaker"].get<std::string>(), std::string("参加者"));
    const auto by_source = body_of(call(app, "GET", "/api/admin/noise/utterances?source=steam"));
    CHECK_EQ(by_source.size(), 1u);
    CHECK_EQ(by_source[0]["speaker"].get<std::string>(), std::string("zz"));
    CHECK_EQ(call(app, "POST", "/api/admin/noise/restore", R"({"utteranceId":"u2"})").status, 200);
    CHECK_EQ(body_of(call(app, "GET", "/api/admin/noise/utterances?sessionId=s1"))[1]["excluded"].get<bool>(), false);
    // 巨大なクエリ値でも落ちない (一致なし)
    CHECK_EQ(body_of(call(app, "GET", "/api/admin/noise/utterances?sessionId=" + std::string(100'000, 'x'))).size(), 0u);
}

// ── datasource routes ───────────────────────────────────────────────────────

TEST_CASE("datasource routes: 一覧・発話 (仮名マスク・生 ID は admin)・除外 (台帳に印)・active-kg の書き戻し") {
    Env e(Json{{"discatier", Json{{"knowledgeGraphs", Json::array({Json{{"id", "default"}, {"label", "既定"}, {"kuzuPath", "PLACEHOLDER"}},
                                                                   Json{{"id", "task2"}, {"label", "別"}, {"kuzuPath", "./kg2.sqlite"}}})}}}});
    // knowledgeGraphs の kuzuPath を一時 KG に向け直す (env の DISCATIER_KUZU_PATH は旧単数経路なので使わない)。
    {
        auto cfg = parse_json_or_throw(*fs::read_file(e.config_file));
        cfg["discatier"]["knowledgeGraphs"][0]["kuzuPath"] = e.kg_path;
        CHECK(fs::write_file_atomic(e.config_file, json_stringify_pretty(cfg, 2) + "\n"));
        config::reset_config_for_test();
    }
    {
        auto c = e.core();
        insert_session(*c, "s1", "steam:alpha");
        insert_utterance(*c, "ext:steam:r1", "s1", std::string("ext:steam:76561198000000001"), std::string(300, 'a'), 10);
        insert_utterance(*c, "ext:steam:r2", "s1", std::string("ext:steam:76561198000000002"), "短い", 20);
        c->close();
        auto attr = crawler::sources::open_attribution_store();
        attr->set({"ext:steam:r1", "steam", "https://steamcommunity.com/profiles/1/recommended/10/", "r1", std::string("alpha"), 5});
        attr->set({"ext:steam:r2", "steam", "https://steamcommunity.com/profiles/2/recommended/10/", "r2", std::string("beta"), 6});
        attr->close();
    }
    auto app = mounted(datasource_routes());
    const auto overview = body_of(call(app, "GET", "/api/admin/datasources"));
    CHECK_EQ(overview["activeKg"].get<std::string>(), std::string("default"));
    CHECK_EQ(overview["knowledgeGraphs"].size(), 2u);
    CHECK_EQ(overview["sources"][0]["source"].get<std::string>(), std::string("steam"));
    CHECK_EQ(overview["sources"][0]["count"].get<int>(), 2);

    const auto items = body_of(call(app, "GET", "/api/admin/datasources/steam/utterances?limit=abc"));
    CHECK_EQ(items["items"].size(), 2u);
    CHECK(items["gameSlug"].is_null());
    for (const auto& it : items["items"]) {
        CHECK(str::starts_with(it["speaker"].get<std::string>(), "論者#"));
        CHECK(!str::contains(it["speaker"].get<std::string>(), "7656119"));
        CHECK(str::starts_with(it["speakerId"].get<std::string>(), "ext:steam:"));
    }
    const auto alpha = body_of(call(app, "GET", "/api/admin/datasources/steam/utterances?gameSlug=alpha&limit=1"));
    CHECK_EQ(alpha["items"].size(), 1u);
    CHECK_EQ(alpha["items"][0]["excerpt"].get<std::string>(), std::string(280, 'a') + "…");

    CHECK_EQ(call(app, "POST", "/api/admin/datasources/exclude", "{}").status, 400);
    CHECK_EQ(call(app, "POST", "/api/admin/datasources/exclude", "null").status, 500);
    const auto ex = body_of(call(app, "POST", "/api/admin/datasources/exclude", R"({"source":"steam","gameSlug":"alpha"})"));
    CHECK_EQ(ex["excluded"].get<int>(), 1);
    const auto ex2 = body_of(call(app, "POST", "/api/admin/datasources/exclude", R"({"utteranceId":"unknown-id"})"));
    CHECK_EQ(ex2["excluded"].get<int>(), 1);
    {
        auto c = e.core();
        CHECK(core::noise::is_excluded(c->raw(), "ext:steam:r1"));
        CHECK(!core::noise::is_excluded(c->raw(), "ext:steam:r2"));
        CHECK(core::noise::is_excluded(c->raw(), "unknown-id"));
        c->close();
        auto ing = crawler::sources::open_ingested_store();
        CHECK(ing->has("steam", "r1"));
        ing->close();
    }
    CHECK(body_of(call(app, "GET", "/api/admin/datasources/steam/utterances?gameSlug=alpha"))["items"][0]["excluded"].get<bool>());

    CHECK_EQ(call(app, "POST", "/api/admin/datasources/active-kg", "{}").status, 400);
    const auto unknown = call(app, "POST", "/api/admin/datasources/active-kg", R"({"id":"nope"})");
    CHECK_EQ(unknown.status, 400);
    W3O_SAME(body_of(unknown)["knownIds"], (Json::array({"default", "task2"})));
    const auto switched = body_of(call(app, "POST", "/api/admin/datasources/active-kg", R"({"id":"task2"})"));
    W3O_SAME(switched, (Json{{"ok", true}, {"activeKg", "task2"}, {"restartRequired", true}}));
    const auto file = parse_json_or_throw(*fs::read_file(e.config_file));
    CHECK_EQ(file["discatier"]["activeKg"].get<std::string>(), std::string("task2"));
    CHECK_EQ(file["discatier"]["knowledgeGraphs"].size(), 2u);
    // 即時切替はしない (キャッシュ済み config は default のまま)
    CHECK_EQ(body_of(call(app, "GET", "/api/admin/datasources"))["activeKg"].get<std::string>(), std::string("default"));
}

TEST_CASE("境界: 壊れた config ファイルは書き戻さず 500 (中身を壊さない)") {
    Env e;
    CHECK(fs::write_file_atomic(e.config_file, "{ broken"));
    auto app = mounted(persona_routes());
    const auto r = call(app, "PUT", "/api/admin/personas/auto-adopt", R"({"enabled":true})");
    CHECK_EQ(r.status, 500);
    CHECK(str::starts_with(body_of(r)["error"].get<std::string>(), "config write failed: "));
    CHECK_EQ(*fs::read_file(e.config_file), std::string("{ broken"));
}

// ── queue routes ────────────────────────────────────────────────────────────

TEST_CASE("queue routes: persona-engine DB が無くてもスナップショットを返す") {
    Env e;
    {
        auto c = e.core();
        c->raw().run("INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g1', 'knowledge', '未解決', 'open', 1, 1)");
        c->close();
    }
    auto app = mounted(queue_routes());
    const auto r = call(app, "GET", "/api/admin/queue");
    CHECK_EQ(r.status, 200);
    const auto body = body_of(r);
    CHECK_EQ(body["workspaceId"].get<std::string>(), std::string("knowledge"));
    CHECK_EQ(body["openGaps"].size(), 1u);
    CHECK(!fs::exists(e.dir.path + "/persona-engine.db"));  // 読み取り専用で開くだけ (作らない)
}

// ── kg-migration routes ─────────────────────────────────────────────────────

TEST_CASE("kg-migration: serveToken 未設定は誰でも取得・設定時は ?token= か x-kg-token (定数時間比較)") {
    Json seen = Json::array();
    KgMigrationRouteDeps open;
    open.serveToken = [] { return std::optional<std::string>(); };
    open.exportShared = [&seen](double since) {
        seen.push_back(since);
        kg_sync::KgExportResult r;
        r.ndjson = "{\"manifest\":1}\n";
        r.watermark = 1234.5;
        r.count = 3;
        return r;
    };
    auto app = mounted(kg_migration_routes(open));
    auto r = call(app, "GET", "/api/kg/migrations", "", "", "");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.headers.get("content-type").value_or(""), std::string("application/x-ndjson; charset=utf-8"));
    CHECK_EQ(r.headers.get("x-kg-watermark").value_or(""), std::string("1234.5"));
    CHECK_EQ(r.headers.get("x-kg-count").value_or(""), std::string("3"));
    CHECK_EQ(r.body, std::string("{\"manifest\":1}\n"));
    CHECK_EQ(call(app, "GET", "/api/kg/migrations?since=10").status, 200);
    CHECK_EQ(call(app, "GET", "/api/kg/migrations?since=").status, 200);
    CHECK_EQ(call(app, "GET", "/api/kg/migrations?since=-1").status, 400);
    CHECK_EQ(call(app, "GET", "/api/kg/migrations?since=abc").status, 400);
    CHECK_EQ(call(app, "GET", "/api/kg/migrations?since=1e400").status, 400);
    CHECK_EQ(call(app, "GET", "/api/kg/migrations?since=0x10").status, 200);
    W3O_SAME(seen, (Json::array({0, 10, 0, 16})));

    KgMigrationRouteDeps locked = open;
    locked.serveToken = [] { return std::optional<std::string>("serve-secret-for-tests"); };
    auto guarded = mounted(kg_migration_routes(locked));
    const auto denied = call(guarded, "GET", "/api/kg/migrations");
    CHECK_EQ(denied.status, 403);
    CHECK_EQ(denied.body, std::string("forbidden"));
    CHECK_EQ(call(guarded, "GET", "/api/kg/migrations?token=serve-secret-for-tests").status, 200);
    CHECK_EQ(call(guarded, "GET", "/api/kg/migrations?token=serve-secret-for-test").status, 403);
    CHECK_EQ(call(guarded, "GET", "/api/kg/migrations?token=serve-secret-for-testss").status, 403);
    CHECK_EQ(call(guarded, "GET", "/api/kg/migrations?token=" + std::string(1u << 16, 's')).status, 403);
    {
        http::Request req;
        req.method = "GET";
        req.target = req.path = "/api/kg/migrations";
        req.headers.set("x-kg-token", "serve-secret-for-tests");
        CHECK_EQ(guarded.handle(req).status, 200);
        // ?token= があれば (空でも) そちらを使う (TS の ?? と同じ)
        req.target = "/api/kg/migrations?token=";
        req.query_string = "token=";
        CHECK_EQ(guarded.handle(req).status, 403);
    }
}

TEST_CASE("kg-migration: 既定の export は active KG の共有テーブルを読む") {
    Env e;
    auto app = mounted(kg_migration_routes({[] { return std::optional<std::string>(); }, {}}));
    const auto r = call(app, "GET", "/api/kg/migrations?since=0");
    CHECK_EQ(r.status, 200);
    CHECK(str::contains(r.body, "manifest"));
    CHECK_EQ(r.headers.get("x-kg-count").value_or(""), std::string("0"));
}

DI_TEST_MAIN()
