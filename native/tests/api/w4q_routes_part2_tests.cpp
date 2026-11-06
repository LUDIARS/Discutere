// w4q_routes のテスト (2/4)。準備は w4q_routes_support.hpp。
#include "w4q_routes_support.hpp"

using namespace w4q_routes_support;

// ─── 結論一覧は conclusion_cache だけを読む ─────────────────────────────────

TEST_CASE("learning: キャッシュがあれば KG を開かない (opener が呼ばれない)") {
    w4q::Env env;
    auto db = flow::db::get_flow_db();
    visualize::ConclusionSummary s;
    s.gapId = "gap-x";
    s.kind = "gap";
    s.sessionId = "s";
    s.title = "t";
    s.conclusion = "c";
    s.utteranceCount = 3;
    s.updatedAt = 4000;
    visualize::UpsertOptions o;
    o.now = 1;
    visualize::upsert_conclusion_cache(*db, s, o);
    std::atomic<int> opened{0};
    api::LearningViewDeps deps;
    deps.openCore = [&opened]() -> std::unique_ptr<core::Core> {
        ++opened;
        throw std::runtime_error("KG must not be opened");
    };
    const auto root = root_app(env, deps);
    const auto res = call(root, "GET", "/learning/conclusions?limit=5");
    CHECK_EQ(res.status, 200);
    const auto body = body_json(res);
    CHECK_EQ(body["cached"].get<bool>(), true);
    CHECK_EQ(body["conclusions"][0]["gapId"].get<std::string>(), std::string("gap-x"));
    CHECK_EQ(opened.load(), 0);
    // 小数の limit は TS と同じく 500 (SQLite の datatype mismatch)
    CHECK_EQ(call(root, "GET", "/learning/conclusions?limit=1.5").status, 500);
}

TEST_CASE("learning: md export のファイル名は encodeURIComponent、Fundamentum export は store に put") {
    const auto fx = w4q::load_fixture("routes.json");
    w4q::Env env;
    env.flow_exec(fx["flowSetup"]);
    std::shared_ptr<lib::fundamentum::Foundation> store = lib::fundamentum::Foundation::in_memory();
    fundamentum_export::set_fundamentum_store_for_tests(store);
    const auto root = root_app(env);
    auto res = call(root, "POST", "/learning/conclusion/export/fundamentum?gap=flow:fs-1");
    CHECK_EQ(res.status, 200);
    auto body = body_json(res);
    CHECK_EQ(body["discussionId"].get<std::string>(), std::string("flow:fs-1"));
    // 内容が同じなら再 export は同じ content id
    const auto again = body_json(call(root, "POST", "/learning/conclusion/export/fundamentum?gap=flow:fs-1"));
    CHECK_EQ(again["contentId"], body["contentId"]);
    CHECK_EQ(call(root, "POST", "/learning/conclusion/export/fundamentum?gap=flow:none").status, 404);
    fundamentum_export::set_fundamentum_store_for_tests(nullptr);
}

// ─── consensus の認可 ───────────────────────────────────────────────────

TEST_CASE("consensus: 全ルートで 401 (userId 無し) / 403 (role≠admin)、loopback は admin") {
    w4q::Env env;
    const auto api = api_app();
    for (const char* target : {"/api/admin/consensus", "/api/admin/consensus/gaps", "/api/admin/consensus/g1"}) {
        auto r = call(api, "GET", target, Json{{"X-User-Id", "u1"}});
        CHECK_EQ(r.status, 403);
        CHECK_EQ(body_json(r)["error"].get<std::string>(), std::string("admin role required"));
        r = call(api, "GET", target, Json{{"X-User-Id", "u1"}, {"X-User-Role", "Admin"}});
        CHECK_EQ(r.status, 403);
        CHECK_EQ(call(api, "GET", target, Json::object(), "", "192.168.1.10").status, 403);
        CHECK_EQ(call(api, "GET", target, Json::object(), "", "127.0.0.1").status, 200);
    }
    // userContext を通らない (userId が空) と 401
    http::Router bare;
    bare.mount("/api", api::consensus_routes());
    const auto r = call(bare, "GET", "/api/admin/consensus/gaps");
    CHECK_EQ(r.status, 401);
    CHECK_EQ(body_json(r)["error"].get<std::string>(), std::string("Authentication required"));
    // w3-M の persona / gap 作成ルートも同じ Router に載っている
    CHECK_EQ(call(api, "GET", "/api/admin/consensus/g1/personas", Json{{"X-User-Id", "u1"}}).status, 403);
    CHECK_EQ(call(api, "POST", "/api/admin/gaps", Json{{"X-User-Id", "u1"}}).status, 403);
}

DI_TEST_MAIN()
