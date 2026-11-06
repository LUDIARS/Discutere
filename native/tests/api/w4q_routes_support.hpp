#pragma once
// w4q_routes_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
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

namespace w4q_routes_support {

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

} // namespace w4q_routes_support










// ─── blackbox ──────────────────────────────────────────────────────────

namespace w4q_routes_support {

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

} // namespace w4q_routes_support
