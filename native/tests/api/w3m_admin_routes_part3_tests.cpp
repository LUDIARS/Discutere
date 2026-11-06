// w3m_admin_routes のテスト (3/3)。準備は w3m_admin_routes_support.hpp。
#include "w3m_admin_routes_support.hpp"

using namespace w3m_admin_routes_support;

TEST_CASE("本文の上限: 既定 1 MiB ちょうどは受け、+1 はルータが拒否する口 (max_body_for)・巨大でも壊れた JSON 扱い") {
    Env env;
    const auto r = app();
    CHECK_EQ(r.max_body_for("POST", "/api/admin/rules/enabled"), http::kDefaultMaxBody);
    CHECK_EQ(r.max_body_for("POST", "/api/admin/gaps"), http::kDefaultMaxBody);
    auto engine = make_engine(env.pe);
    api::set_persona_engine(engine);
    // ちょうど上限の JSON 本文
    std::string exact = R"({"enabled":false,"pad":")";
    exact += std::string(http::kDefaultMaxBody - exact.size() - 2, 'a') + "\"}";
    CHECK_EQ(exact.size(), http::kDefaultMaxBody);
    CHECK_EQ(call(r, "POST", "/api/admin/rules/enabled", exact).body, std::string(R"({"ok":true,"enabled":false})"));
    // 深い入れ子 (parse_json の上限) は壊れた JSON 扱い
    CHECK_EQ(call(r, "POST", "/api/admin/rules/enabled", str::repeat("[", 5000) + str::repeat("]", 5000)).body,
             std::string(R"({"ok":true,"enabled":true})"));
}

TEST_CASE("POST /admin/gaps: 415・400 (各種)・201・gameId の存在確認・DB 失敗は 503 で詳細を出さない") {
    Env env;
    const auto r = app();
    const Json json_ct{{"content-type", "Application/JSON; charset=utf-8"}};
    auto res = call(r, "POST", "/api/admin/gaps", R"({"title":"t","description":"d"})", Json{{"content-type", "text/plain"}});
    CHECK_EQ(res.status, 415);
    CHECK_EQ(call(r, "POST", "/api/admin/gaps", R"({"title":"t","description":"d"})").status, 415);  // ヘッダ無し
    for (const std::string& body : {std::string("broken"), std::string("[]"), std::string("null"), std::string(R"({"title":" ","description":"d"})"),
                                   std::string(R"({"title":"t"})"), std::string(R"({"title":"t","description":"d","gameId":""})"),
                                   std::string(R"({"title":"t","description":"d","gameId":null})"), std::string(R"({"title":1,"description":"d"})")}) {
        res = call(r, "POST", "/api/admin/gaps", body, json_ct);
        CHECK_EQ(res.status, 400);
        CHECK_EQ(body_json(res)["error"], Json("title and description are required; gameId must be non-empty when supplied"));
    }
    res = call(r, "POST", "/api/admin/gaps", R"({"title":"  議題  ","description":" 説明 "})", json_ct);
    CHECK_EQ(res.status, 201);
    const auto j = body_json(res);
    std::vector<std::string> keys;
    for (auto it = j.begin(); it != j.end(); ++it) keys.push_back(it.key());
    CHECK_EQ(str::join(keys, ","), std::string("gapId,sessionId"));
    auto c = env.core();
    const auto gap = c->raw().get("SELECT * FROM design_gaps WHERE id = ?", {j["gapId"].get<std::string>()});
    CHECK_EQ(gap->str("title"), std::string("議題"));
    CHECK_EQ(gap->str("evidence_json"), std::string(R"({"source":"api:admin-gap"})"));
    res = call(r, "POST", "/api/admin/gaps", R"({"title":"t","description":"d","gameId":"nope"})", json_ct);
    CHECK_EQ(res.status, 400);
    CHECK_EQ(body_json(res)["error"], Json("gameId must identify a game in the current workspace"));
    core::repositories::GameInput g;
    g.workspaceId = WS;
    g.title = "架空ゲーム";
    const auto game_id = c->repos.game.create(g);
    c.reset();
    res = call(r, "POST", "/api/admin/gaps", R"({"title":"t","description":"d","gameId":")" + game_id + "\"}", json_ct);
    CHECK_EQ(res.status, 201);

    // persona-engine DB を開けない (ディレクトリを指す) → 503、パスを出さない
    env::set_for_test("DISCUTERE_PERSONA_ENGINE_DB", std::string_view(env.dir.path.string()));
    config::reset_config_for_test();
    res = call(r, "POST", "/api/admin/gaps", R"({"title":"t2","description":"d"})", json_ct);
    CHECK_EQ(res.status, 503);
    CHECK_EQ(res.body, std::string(R"({"error":"gap creation unavailable"})"));
}

TEST_CASE("GET /admin/consensus/:gapId/personas: session 無しは空・集計・persona DB 不在は 503 (Cache-Control 付き)") {
    Env env;
    const auto r = app();
    auto res = call(r, "GET", "/api/admin/consensus/none/personas");
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, std::string(R"({"gapId":"none","personas":[]})"));
    CHECK_EQ(res.headers.get("Cache-Control").value_or(""), std::string("private, no-store"));

    auto c = env.core();
    const auto gap = make_gap(*c, "合意", "gap:c");
    // consensus_scores / opinion_scores の表を用意
    {
        auto fake_llm = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{});
        persona_engine::scoring::ConsensusScorerDeps sd;
        sd.core = std::shared_ptr<core::Core>(c.get(), [](core::Core*) {});
        sd.llm = fake_llm;
        persona_engine::scoring::ConsensusScorer scorer(sd);
    }
    auto say = [&](const std::string& sp, const std::string& text, double at) {
        core::repositories::UtteranceInput u;
        u.workspaceId = WS;
        u.sessionId = gap.sessionId;
        u.speakerId = sp;
        u.rawContent = text;
        u.postedAt = at;
        return c->repos.utterance.create(u);
    };
    const auto u1 = say("persona:p1", "意見1", 10);
    say("persona:facilitator", "進行", 11);
    say("persona:ghost", "未登録", 12);
    say("user-1", "人間", 13);
    c->raw().run("INSERT INTO consensus_scores (utterance_id, agree, score, reasoning, created_at) VALUES (?, 1, 4, 'r', 1)", {u1});
    c.reset();
    // persona DB が無い → 503
    res = call(r, "GET", "/api/admin/consensus/" + gap.gapId + "/personas");
    CHECK_EQ(res.status, 503);
    CHECK_EQ(res.body, std::string(R"({"error":"persona consensus unavailable"})"));
    CHECK_EQ(res.headers.get("Cache-Control").value_or(""), std::string("private, no-store"));
    CHECK(!std::filesystem::exists(env.pe));  // readonly / fileMustExist で作らない

    auto pe = sql::Database::open(env.pe);
    persona_engine::db::apply_persona_engine_migrations(*pe);
    persona_engine::db::PersonasRepo repo(pe);
    repo.insert_or_ignore({"p1", "論客", "論 客", "", {"冷静"}, ""});
    pe->run("UPDATE personas SET traits = 'not json' WHERE id = 'p1'");
    res = call(r, "GET", "/api/admin/consensus/" + gap.gapId + "/personas");
    CHECK_EQ(res.status, 200);
    const auto j = body_json(res);
    CHECK_EQ(j["personas"].size(), std::size_t(1));
    CHECK_EQ(j["personas"][0]["personaId"], Json("p1"));
    CHECK_EQ(j["personas"][0]["traits"], Json::array());  // 壊れた traits は []
    CHECK_EQ(j["personas"][0]["utterances"][0]["agree"], Json(true));
}

DI_TEST_MAIN()
