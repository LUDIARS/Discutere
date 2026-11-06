// w3m_admin_routes のテスト (1/3)。準備は w3m_admin_routes_support.hpp。
#include "w3m_admin_routes_support.hpp"

using namespace w3m_admin_routes_support;

TEST_CASE("認可: 非 loopback の匿名・role≠admin は 403、userId 無し (middleware 無し) は 401") {
    Env env;
    const auto r = app();
    for (const auto& [m, path] : kAdminRoutes) {
        auto res = call(r, m, path, "{}", Json{{"content-type", "application/json"}}, "203.0.113.5");
        CHECK_EQ(res.status, 403);
        CHECK_EQ(body_json(res)["error"], Json("admin role required"));
        res = call(r, m, path, "{}", Json{{"X-User-Id", "u1"}, {"X-User-Role", "general"}, {"content-type", "application/json"}});
        CHECK_EQ(res.status, 403);
        // consensus routes は guard を通った後にだけ Cache-Control を付ける
        CHECK(!res.headers.has("Cache-Control"));
    }
    http::Router bare;
    bare.mount("/api", api::admin_routes());
    bare.mount("/api", api::consensus_persona_routes());
    bare.mount("/api", api::gap_creation_routes());
    for (const auto& [m, path] : kAdminRoutes) {
        const auto res = call(bare, m, path, "{}", Json{{"content-type", "application/json"}});
        CHECK_EQ(res.status, 401);
        CHECK_EQ(body_json(res)["error"], Json("Authentication required"));
    }
}

TEST_CASE("late-binding: engine / facilitator / sweeper 未注入は 503、status は未接続を返す") {
    Env env;
    const auto r = app();
    for (const auto& path : {"/api/admin/rules/enabled", "/api/admin/session/reset"}) {
        const auto res = call(r, "POST", path, "{}");
        CHECK_EQ(res.status, 503);
        CHECK_EQ(body_json(res)["error"], Json("persona-engine not initialized"));
    }
    for (const auto& path : {"/api/admin/metrics/personas", "/api/admin/metrics/rules", "/api/admin/rules", "/api/admin/rules/x/logs"})
        CHECK_EQ(call(r, "GET", path).status, 503);
    auto res = call(r, "POST", "/api/admin/sessions/s/converge");
    CHECK_EQ(res.status, 503);
    CHECK_EQ(body_json(res)["error"], Json("facilitator not initialized"));
    res = call(r, "POST", "/api/admin/seed-sweep", "{}");
    CHECK_EQ(res.status, 503);
    CHECK_EQ(body_json(res)["error"], Json("gateway not initialized"));
    res = call(r, "GET", "/api/admin/status");
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, std::string(R"({"persona_engine_attached":false,"rules_runtime_enabled":true,"persona_count":0,"rule_count":0,"recent_rule_logs":[]})"));
}

TEST_CASE("engine: rules/enabled・session/reset・status・metrics・rules・logs") {
    Env env;
    auto engine = make_engine(env.pe);
    api::set_persona_engine(engine);
    CHECK(api::get_persona_engine() == engine);
    const auto r = app();

    auto res = call(r, "POST", "/api/admin/rules/enabled", R"({"enabled":false})");
    CHECK_EQ(res.body, std::string(R"({"ok":true,"enabled":false})"));
    res = call(r, "GET", "/api/admin/status");
    auto st = body_json(res);
    CHECK_EQ(st["persona_engine_attached"], Json(true));
    CHECK_EQ(st["rules_runtime_enabled"], Json(false));
    CHECK(st["persona_count"].get<int>() > 0);
    CHECK(st["rule_count"].get<int>() > 0);
    // 壊れた JSON は {enabled:true} とみなす (TS の .catch)
    res = call(r, "POST", "/api/admin/rules/enabled", "{broken");
    CHECK_EQ(res.body, std::string(R"({"ok":true,"enabled":true})"));
    // 真偽は JS の truthy
    CHECK_EQ(call(r, "POST", "/api/admin/rules/enabled", R"({"enabled":0})").body, std::string(R"({"ok":true,"enabled":false})"));
    CHECK_EQ(call(r, "POST", "/api/admin/rules/enabled", R"({"enabled":"x"})").body, std::string(R"({"ok":true,"enabled":true})"));
    CHECK_EQ(call(r, "POST", "/api/admin/rules/enabled", R"([1])").body, std::string(R"({"ok":true,"enabled":false})"));

    res = call(r, "POST", "/api/admin/session/reset", R"({"sessionId":""})");
    CHECK_EQ(res.status, 400);
    CHECK_EQ(body_json(res)["error"], Json("sessionId required"));
    CHECK_EQ(call(r, "POST", "/api/admin/session/reset", "not json").status, 400);
    res = call(r, "POST", "/api/admin/session/reset", R"({"sessionId":"s-1"})");
    CHECK_EQ(res.body, std::string(R"({"ok":true,"sessionId":"s-1"})"));

    res = call(r, "GET", "/api/admin/metrics/personas");
    CHECK_EQ(res.status, 200);
    CHECK(body_json(res)["metrics"].is_array());
    CHECK(body_json(call(r, "GET", "/api/admin/metrics/rules"))["metrics"].is_array());

    res = call(r, "GET", "/api/admin/rules");
    const auto rules = body_json(res)["rules"];
    CHECK(!rules.empty());
    const auto& first = rules[0];
    std::vector<std::string> keys;
    for (auto it = first.begin(); it != first.end(); ++it) keys.push_back(it.key());
    CHECK_EQ(str::join(keys, ","), std::string("id,description,trigger_type,tick_sec,event_kind,target,cooldown_sec,last_fired_at,enabled,added_at,"
                                               "added_by,removed_at,removed_by,removed_reason,is_seed,is_ai_added,is_ai_removed"));
    CHECK_EQ(first["is_seed"], Json(true));
    const auto id = first["id"].get<std::string>();
    engine->rules().log({id, "fire", "engine", std::string("d1")});
    engine->rules().log({id, "skip", "engine", std::string("d2")});
    engine->rules().log({std::string("other"), "fire", "engine", std::nullopt});
    res = call(r, "GET", "/api/admin/rules/" + id + "/logs?limit=1");
    auto logs = body_json(res);
    CHECK_EQ(logs["rule_id"], Json(id));
    CHECK_EQ(logs["logs"].size(), std::size_t(1));
    CHECK_EQ(body_json(call(r, "GET", "/api/admin/rules/" + id + "/logs"))["logs"].size(), std::size_t(2));
    // 境界: limit=0 → 1、NaN → 0 件、巨大 → 200 にクランプ、小数は切り捨て
    CHECK_EQ(body_json(call(r, "GET", "/api/admin/rules/" + id + "/logs?limit=0"))["logs"].size(), std::size_t(1));
    CHECK_EQ(body_json(call(r, "GET", "/api/admin/rules/" + id + "/logs?limit=abc"))["logs"].size(), std::size_t(0));
    CHECK_EQ(body_json(call(r, "GET", "/api/admin/rules/" + id + "/logs?limit=1e9"))["logs"].size(), std::size_t(2));
    CHECK_EQ(body_json(call(r, "GET", "/api/admin/rules/" + id + "/logs?limit=1.9"))["logs"].size(), std::size_t(1));
}

DI_TEST_MAIN()
