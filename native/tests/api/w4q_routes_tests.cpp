// w4-Q: api/{blackbox,consensus,learning-view,web-chat}-routes.ts の移植の検証 (TS テスト無し)。
// golden: 合成データ (fixture の setup SQL) に対する Node の応答 (status / Content-Type / 本文) と一致。
// 認可 (401/403)・結論一覧が conclusion_cache だけを読む (KG を開かない)・web-chat の本文の境界・blackbox の入力検証。
#include "check.hpp"
#include "w4q_support.hpp"

#include <atomic>

#include "api/blackbox_routes.hpp"
#include "api/consensus_routes.hpp"
#include "api/learning_view_routes.hpp"
#include "api/web_chat_routes.hpp"
#include "base/crypto.hpp"
#include "base/web_assets.hpp"
#include "flow/density_blackbox.hpp"
#include "base/strings.hpp"
#include "fundamentum_export/index.hpp"
#include "lib/blackbox/blackbox.hpp"
#include "lib/fundamentum/fundamentum.hpp"
#include "middleware/auth.hpp"
#include "visualize/conclusion_cache.hpp"

using namespace di;
using w4q::body_json;
using w4q::make_request;

namespace {

const Json ADMIN = Json{{"X-User-Id", "golden"}, {"X-User-Role", "admin"}};

http::Router api_app(api::CoreOpener opener = nullptr) {
    http::Router r;
    r.use(middleware::user_context());
    r.mount("/api", api::consensus_routes(std::move(opener)));
    return r;
}

/** learning-cache は一時ディレクトリの存在しないパスへ (data/ を読まない)。 */
http::Router root_app(const w4q::Env& env, api::LearningViewDeps deps = {}) {
    if (!deps.learningCachePath) deps.learningCachePath = env.dir.file("learning-cache.sqlite");
    if (!deps.openAttribution) {
        const auto path = env.dir.file("attribution.sqlite");
        deps.openAttribution = [path] { return crawler::sources::open_attribution_store(path); };
    }
    http::Router r;
    r.mount("/", api::web_chat_routes());
    r.mount("/", api::learning_view_routes(std::move(deps)));
    return r;
}

http::Response call(const http::Router& r, const std::string& method, const std::string& target, const Json& headers = Json::object(),
                    std::string body = "", const std::string& remote = "") {
    auto req = make_request(method, target, headers, remote, std::move(body));
    return r.handle(req);
}

std::string content_type(const http::Response& r) { return r.headers.get("Content-Type").value_or(""); }

void check_golden_body(const http::Response& res, const Json& expected, const std::string& what) {
    if (expected.is_object() && expected.contains("sha256") && expected.size() == 1) {
        CHECK_EQ(crypto::sha256_hex(res.body), expected["sha256"].get<std::string>());
        return;
    }
    if (expected.is_string()) {
        CHECK_EQ(res.body, expected.get<std::string>());
        return;
    }
    auto actual = body_json(res);
    if (actual.is_object() && actual.contains("generatedAt")) actual["generatedAt"] = "<now>";
    if (json_stringify(actual) != json_stringify(expected))
        throw test::Failure(what + "\n  actual:   " + json_stringify(actual) + "\n  expected: " + json_stringify(expected));
}

} // namespace

// ─── golden ──────────────────────────────────────────────────────────────

TEST_CASE("golden: 画面 (consensus / chat / learning) は Node が返した HTML と同じ") {
    w4q::Env env;
    const auto assets = w4q::load_fixture("web_assets.json");
    for (const auto& a : assets) {
        const auto* asset = web::find(a["name"].get<std::string>());
        CHECK(asset != nullptr);
        CHECK_EQ(crypto::sha256_hex(asset->body), a["sha256"].get<std::string>());
    }
    const auto api = api_app();
    const auto html = call(api, "GET", "/api/admin/consensus", ADMIN);
    CHECK_EQ(html.status, 200);
    CHECK_EQ(content_type(html), std::string("text/html; charset=UTF-8"));
    const auto root = root_app(env);
    CHECK_EQ(crypto::sha256_hex(call(root, "GET", "/chat").body), assets[1]["sha256"].get<std::string>());
    CHECK_EQ(crypto::sha256_hex(call(root, "GET", "/learning").body), assets[2]["sha256"].get<std::string>());
}

TEST_CASE("golden: 合成データに対するルートの応答が Node と一致") {
    const auto fx = w4q::load_fixture("routes.json");
    w4q::Env env;
    env.kg_exec(fx["kgSetup"]);
    env.flow_exec(fx["flowSetup"]);
    api::WebChatDeps chat;
    chat.workspaceId = "ws";
    api::set_web_chat_deps(chat);
    const auto api = api_app();
    const auto root = root_app(env);
    for (const auto& rc : fx["routes"]) {
        const auto& app = rc["app"].get<std::string>() == "api" ? api : root;
        const Json headers = rc.contains("headers") ? rc["headers"] : Json::object();
        const auto res = call(app, rc["method"].get<std::string>(), rc["target"].get<std::string>(), headers);
        const auto name = rc["name"].get<std::string>();
        if (res.status != rc["status"].get<int>()) throw test::Failure(name + ": status " + std::to_string(res.status) + "\n" + res.body);
        if (rc["contentType"].is_string()) CHECK_EQ(content_type(res), rc["contentType"].get<std::string>());
        if (rc["contentDisposition"].is_string())
            CHECK_EQ(res.headers.get("Content-Disposition").value_or(""), rc["contentDisposition"].get<std::string>());
        check_golden_body(res, rc["body"], name);
    }
    api::set_web_chat_deps({});
}

TEST_CASE("golden: キャッシュ空のときだけ live マージ (limit の負値・NaN も Node と同じ)") {
    const auto fx = w4q::load_fixture("routes.json");
    for (const auto& f : fx["fallback"]) {
        w4q::Env env;
        env.kg_exec(fx["kgSetup"]);
        const auto root = root_app(env);
        const auto res = call(root, "GET", f["target"].get<std::string>());
        CHECK_EQ(res.status, f["status"].get<int>());
        check_golden_body(res, f["body"], f["target"].get<std::string>());
    }
}

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

// ─── web-chat ──────────────────────────────────────────────────────────

TEST_CASE("web-chat: POST の入力検証・コマンド・分類器の起動と二重起動の防止") {
    w4q::Env env;
    std::atomic<int> classified{0};
    std::string last_title;
    api::WebChatDeps deps;
    deps.workspaceId = "ws";
    deps.classifyInboundMessage = [&](const discord_hook::DiscordAutoDiscussionInput& in) {
        ++classified;
        last_title = in.forumTitle.value_or("");
        CHECK_EQ(in.guildId, std::string("web"));
        // 議論を立てたことにする (scene=web:<room> の discussion-of-gap session)
        auto c = core::create_core(env.kg, env.dir.file("events.jsonl"));
        c->raw().exec("INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('gx', 'ws', 't', 'open', 1, 1)");
        c->raw().exec("INSERT INTO sessions (id, workspace_id, title, started_at, scene, created_at, updated_at) VALUES ('sgx', 'ws', "
                      "'discussion-of-gap:gx', 1, 'web:room1', 1, 1)");
        c->close();
        return discord_hook::StarterResult{true};
    };
    api::set_web_chat_deps(deps);
    const auto root = root_app(env);

    auto r = call(root, "POST", "/api/chat/room1/messages", Json::object(), R"({"text":"   "})");
    CHECK_EQ(r.status, 400);
    CHECK_EQ(json_stringify(body_json(r)), std::string(R"({"ok":false,"error":"text required"})"));
    CHECK_EQ(call(root, "POST", "/api/chat/room1/messages", Json::object(), "{broken").status, 400);  // 壊れた JSON は {} 扱い
    CHECK_EQ(call(root, "POST", "/api/chat/room1/messages", Json::object(), "null").status, 500);     // TS は null.text の TypeError
    CHECK_EQ(call(root, "POST", "/api/chat/room1/messages", Json::object(), R"({"text":5})").status, 400);

    r = call(root, "POST", "/api/chat/room1/messages", Json::object(), R"({"text":"/nope"})");
    CHECK_EQ(json_stringify(body_json(r)),
             std::string(R"({"ok":true,"room":"room1","seeded":false,"commandResult":{"ok":false,"error":"unknown command: nope"}})"));

    const std::string long_text = str::repeat("あ", 100);
    r = call(root, "POST", "/api/chat/room1/messages", Json::object(),
             json_stringify(Json{{"text", "  " + long_text + "  "}, {"author", "  " + str::repeat("名", 50) + "  "}}));
    CHECK_EQ(r.status, 200);
    auto body = body_json(r);
    CHECK_EQ(body["seeded"].get<bool>(), true);
    CHECK_EQ(classified.load(), 1);
    CHECK_EQ(str::js_length(last_title), std::size_t{80});
    // 表示名は 40 文字まで (個人データは表示名のみ)
    auto c = env.core();
    const auto row = c->raw().get("SELECT speaker_id FROM utterances WHERE id = ?", {body["utteranceId"].get<std::string>()});
    CHECK_EQ(row->str("speaker_id"), str::repeat("名", 40));
    c->close();

    // 議論が立っていれば分類器は呼ばない
    r = call(root, "POST", "/api/chat/room1/messages", Json::object(), R"({"text":"続き"})");
    CHECK_EQ(body_json(r)["seeded"].get<bool>(), false);
    CHECK_EQ(classified.load(), 1);
    // 表示名なしは「参加者」
    c = env.core();
    CHECK_EQ(c->raw().get("SELECT speaker_id FROM utterances WHERE raw_content = '続き'")->str("speaker_id"), std::string("参加者"));
    c->close();

    // 分類器の例外は seeded=false (落ちない)
    deps.classifyInboundMessage = [](const discord_hook::DiscordAutoDiscussionInput&) -> discord_hook::StarterResult {
        throw std::runtime_error("llm down");
    };
    api::set_web_chat_deps(deps);
    r = call(root, "POST", "/api/chat/room2/messages", Json::object(), R"({"text":"別の部屋"})");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(body_json(r)["seeded"].get<bool>(), false);
    api::set_web_chat_deps({});
}

TEST_CASE("web-chat: 本文の上限 (64 KiB ちょうど・+1)・巨大・深い入れ子") {
    w4q::Env env;
    api::WebChatDeps deps;
    deps.workspaceId = "ws";
    api::set_web_chat_deps(deps);
    const auto root = root_app(env);
    CHECK_EQ(root.max_body_for("POST", "/api/chat/room1/messages"), api::kWebChatMaxBody);
    // ちょうど上限の本文は受ける
    const std::string prefix = R"({"text":")";
    const std::string suffix = R"("})";
    const std::string exact = prefix + std::string(api::kWebChatMaxBody - prefix.size() - suffix.size(), 'a') + suffix;
    CHECK_EQ(exact.size(), api::kWebChatMaxBody);
    CHECK_EQ(call(root, "POST", "/api/chat/room1/messages", Json::object(), exact).status, 200);
    // +1 は JSON の上限で読めない → {} 扱い → 400 (サーバは max_body_for で 413 にする)
    const std::string over = prefix + std::string(api::kWebChatMaxBody - prefix.size() - suffix.size() + 1, 'a') + suffix;
    CHECK_EQ(call(root, "POST", "/api/chat/room1/messages", Json::object(), over).status, 400);
    // 深い入れ子も読まない
    CHECK_EQ(call(root, "POST", "/api/chat/room1/messages", Json::object(), std::string(1000, '[') + std::string(1000, ']')).status, 400);
    api::set_web_chat_deps({});
}

TEST_CASE("web-chat: GET の room は正規化、since は parseInt") {
    const auto fx = w4q::load_fixture("routes.json");
    w4q::Env env;
    env.kg_exec(fx["kgSetup"]);
    api::WebChatDeps deps;
    deps.workspaceId = "ws";
    deps.resolveSpeakerName = [](const std::string& id) { return id == "p-a" ? std::string("アリス") : id; };
    api::set_web_chat_deps(deps);
    const auto root = root_app(env);
    auto body = body_json(call(root, "GET", "/api/chat/lobby/messages?since=1500xyz"));
    CHECK_EQ(body["messages"].size(), std::size_t{2});
    CHECK_EQ(body["messages"][0]["speaker"].get<std::string>(), std::string("アリス"));
    body = body_json(call(root, "GET", "/api/chat/" + std::string(200, 'r') + "/messages"));
    CHECK_EQ(str::js_length(body["room"].get<std::string>()), std::size_t{32});
    api::set_web_chat_deps({});
}

// ─── blackbox ──────────────────────────────────────────────────────────

namespace {

struct BlackboxFixture {
    w4q::Env env;
    lib::blackbox::BlackBox box = lib::blackbox::make_memory_blackbox();
    http::Router app;
    BlackboxFixture() {
        api::reset_blackbox_routes();
        app.use(middleware::user_context());
        auto b = box;
        app.mount("/api", api::blackbox_routes([b] { return b; }));
    }
    ~BlackboxFixture() { api::reset_blackbox_routes(); }
    http::Response admin(const std::string& method, const std::string& target, std::string body = "") {
        return call(app, method, target, ADMIN, std::move(body));
    }
};

} // namespace

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
