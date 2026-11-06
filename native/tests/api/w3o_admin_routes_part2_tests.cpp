// w3o_admin_routes のテスト (2/3)。準備は w3o_admin_routes_support.hpp。
#include "w3o_admin_routes_support.hpp"

using namespace w3o_admin_routes_support;

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

DI_TEST_MAIN()
