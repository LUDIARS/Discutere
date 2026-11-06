// w4q_routes のテスト (1/4)。準備は w4q_routes_support.hpp。
#include "w4q_routes_support.hpp"

using namespace w4q_routes_support;

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

DI_TEST_MAIN()
