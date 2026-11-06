#pragma once
// w3o_admin_routes_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
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

namespace w3o_admin_routes_support {

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

} // namespace w3o_admin_routes_support
