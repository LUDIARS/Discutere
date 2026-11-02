// w3-L: api/conclusion_publication_routes の境界テスト (tests/visualize/conclusion-publication.test.ts の観点 + 境界)。
//   - トークン: 32 バイト未満は 503 (31 / 32 / 33 バイト)、Bearer の形、長さ違い・1 文字違いは 401。
//   - ローカル操作: loopback 以外・Host が localhost / 127.0.0.1 / [::1] 以外・Origin 不一致は 403。
//   - PUT 本文: content-type / 形 / キー / 型 / id の長さ (500 ちょうど・501)、壊れた・巨大な本文。
#include "check.hpp"

#include <filesystem>

#include "api/conclusion_publication_routes.hpp"
#include "base/crypto.hpp"
#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "flow/db/migrations.hpp"
#include "integrations/conclusion_publication.hpp"
#include "visualize/conclusion_cache.hpp"

using namespace di;

namespace {

struct Fixture {
    Fixture() {
        dir = (std::filesystem::temp_directory_path() / ("w3l-pub-" + crypto::uuid_v4())).string();
        std::filesystem::create_directories(dir);
        db = sql::Database::open(dir + "/flow.db");
        flow::db::apply_flow_migrations(*db);
        for (const char* id : {"public", "glab", "private", "unset"}) {
            visualize::ConclusionSummary s;
            s.gapId = id;
            s.kind = "gap";
            s.sessionId = id;
            s.title = id;
            s.conclusion = "approved result";
            s.utteranceCount = 1;
            s.updatedAt = 1;
            visualize::upsert_conclusion_cache(*db, s);
        }
        routes = api::create_conclusion_publication_routes([this]() -> sql::Database& { return *db; }, [this] { return token; });
    }
    ~Fixture() {
        db->close();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    http::Response request(const std::string& method, const std::string& target, std::vector<std::pair<std::string, std::string>> headers = {},
                           const std::string& body = "", const std::string& remote = "127.0.0.1") {
        http::Request req;
        req.method = method;
        req.target = target;
        const auto q = target.find('?');
        req.path = target.substr(0, q);
        if (q != std::string::npos) req.query_string = target.substr(q + 1);
        req.remote_address = remote;
        for (auto& [k, v] : headers) req.headers.add(k, v);
        req.body = body;
        return routes.handle(req);
    }
    std::string dir;
    sql::DatabasePtr db;
    std::string token = std::string(40, 'a');
    http::Router routes;
};

std::string error_of(const http::Response& r) {
    const auto j = parse_json(r.body);
    return j && j->is_object() && j->contains("error") ? (*j)["error"].get<std::string>() : "";
}

const std::vector<std::pair<std::string, std::string>> kLocal = {{"host", "localhost:4430"}};

} // namespace

TEST_CASE("glab: トークンが 32 バイト未満なら 503、ちょうど 32 から有効") {
    Fixture f;
    f.token = std::string(31, 'x');
    auto r = f.request("GET", "/integrations/glab/conclusions", {{"authorization", "Bearer " + f.token}});
    CHECK_EQ(r.status, 503);
    CHECK_EQ(error_of(r), std::string("conclusions_unconfigured"));
    CHECK_EQ(r.headers.get("cache-control").value_or(""), std::string("private, no-store"));
    f.token = "";
    CHECK_EQ(f.request("GET", "/integrations/glab/conclusions").status, 503);
    f.token = std::string(32, 'x');
    CHECK_EQ(f.request("GET", "/integrations/glab/conclusions", {{"authorization", "Bearer " + f.token}}).status, 200);
    f.token = std::string(33, 'x');
    CHECK_EQ(f.request("GET", "/integrations/glab/conclusions", {{"authorization", "Bearer " + f.token}}).status, 200);
    // 日本語 3 バイト × 11 = 33 バイト (UTF-16 では 11) の token は 503 にならない (Buffer の長さで数える)。
    // ただし Node はヘッダを latin1 として読むので、UTF-8 のまま送った値は一致しない (401。Node と同じ)。
    f.token = "";
    for (int i = 0; i < 11; ++i) f.token += "鍵";
    CHECK_EQ(f.request("GET", "/integrations/glab/conclusions", {{"authorization", "Bearer " + f.token}}).status, 401);
}

TEST_CASE("glab: Bearer の形・長さ違い・1 文字違いは 401") {
    Fixture f;
    const std::string t = f.token;
    for (const std::string& auth : std::vector<std::string>{std::string(""), "bearer " + t, "Bearer  " + t, "Bearer " + t + " ", "Bearer " + t + "x",
                                    "Bearer " + t.substr(1), "Bearer " + t.substr(0, 39) + "b", "Bearer", "Bearer ", "Basic " + t,
                                    "Bearer " + t.substr(0, 20) + "\xa0" + t.substr(20), "Bearer " + std::string(1u << 16, 'a')}) {
        auto r = f.request("GET", "/integrations/glab/conclusions", {{"authorization", auth}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(error_of(r), std::string("unauthorized"));
    }
    CHECK_EQ(f.request("GET", "/integrations/glab/conclusions").status, 401);
    // 役割ヘッダは信用しない
    CHECK_EQ(f.request("GET", "/integrations/glab/conclusions", {{"X-User-Role", "admin"}}).status, 401);
    // latin1 として読む: 0xE9 の 1 バイトは UTF-8 の "é" (2 バイト) と同じ token になる
    f.token = std::string(31, 'a') + "\xc3\xa9";
    CHECK_EQ(f.request("GET", "/integrations/glab/conclusions", {{"authorization", "Bearer " + std::string(31, 'a') + "\xe9"}}).status, 200);
    CHECK_EQ(api::bearer_token_of(std::string("Bearer a\tb")), std::string(""));
    CHECK_EQ(api::bearer_token_of(std::string("Bearer ab")), std::string("ab"));
    CHECK_EQ(api::bearer_token_of(std::nullopt), std::string(""));
}

TEST_CASE("glab: 公開中だけを返し、?id で無ければ 404") {
    Fixture f;
    const auto auth = std::vector<std::pair<std::string, std::string>>{{"authorization", "Bearer " + f.token}};
    for (const std::string& scope : std::vector<std::string>{"public", "glab", "private"})
        CHECK(integrations::set_publication(*f.db, scope, true, scope, integrations::publication_state(*f.db, scope)->revision, 2));
    auto r = f.request("GET", "/integrations/glab/conclusions", auth);
    CHECK_EQ(r.status, 200);
    const auto body = parse_json_or_throw(r.body);
    CHECK_EQ(body["conclusions"].size(), 2u);
    std::vector<std::string> keys;
    for (const auto& [k, v] : body["conclusions"][0].items()) {
        (void)v;
        keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    CHECK((keys == std::vector<std::string>{"conclusion", "id", "scope", "title", "updatedAt"}));
    CHECK_EQ(f.request("GET", "/integrations/glab/conclusions?id=private", auth).status, 404);
    CHECK_EQ(f.request("GET", "/integrations/glab/conclusions?id=", auth).status, 404);
    CHECK_EQ(f.request("GET", "/integrations/glab/conclusions?id=public", auth).status, 200);
}

TEST_CASE("admin: loopback + Host (localhost / 127.0.0.1 / [::1]) + Origin 一致だけが通る") {
    Fixture f;
    CHECK_EQ(f.request("GET", "/admin/conclusion-publication?id=public", kLocal).status, 200);
    CHECK_EQ(f.request("GET", "/admin/conclusion-publication?id=public", {{"host", "127.0.0.1"}}).status, 200);
    CHECK_EQ(f.request("GET", "/admin/conclusion-publication?id=public", {{"host", "[::1]:4430"}}).status, 200);
    CHECK_EQ(f.request("GET", "/admin/conclusion-publication?id=public", {{"host", "LOCALHOST"}}).status, 200);
    CHECK_EQ(f.request("GET", "/admin/conclusion-publication?id=nope", kLocal).status, 404);
    for (const std::string& host : std::vector<std::string>{"public.example", "localhost.", "localhost.evil.example", "127.0.0.2", "", "localhost:abc",
                                   "localhost:65536", "user@localhost", "[::2]", "local host"}) {
        std::vector<std::pair<std::string, std::string>> h;
        if (!host.empty()) h.push_back({"host", host});
        auto r = f.request("GET", "/admin/conclusion-publication?id=public", h);
        CHECK_EQ(r.status, 403);
        CHECK_EQ(error_of(r), std::string("local_operator_required"));
    }
    // 相手が loopback でない (トンネル・プロキシ越し)
    CHECK_EQ(f.request("GET", "/admin/conclusion-publication?id=public", kLocal, "", "192.168.1.5").status, 403);
    CHECK_EQ(f.request("GET", "/admin/conclusion-publication?id=public", kLocal, "", "").status, 403);
    // 役割ヘッダでは通らない
    CHECK_EQ(f.request("GET", "/admin/conclusion-publication?id=public", {{"host", "public.example"}, {"X-User-Role", "admin"}}).status, 403);
    // 絶対形式の要求行はその URL のホストで判定する (Host ヘッダより優先)
    {
        http::Request req;
        req.method = "GET";
        req.target = "http://public.example/admin/conclusion-publication?id=public";
        req.path = "/admin/conclusion-publication";
        req.query_string = "id=public";
        req.remote_address = "127.0.0.1";
        req.headers.set("host", "localhost");
        CHECK_EQ(f.routes.handle(req).status, 403);
        req.target = "HTTP://[::1]:4430/admin/conclusion-publication?id=public";
        CHECK_EQ(f.routes.handle(req).status, 200);
        const auto p = api::request_url_parts(req);
        CHECK_EQ(p->origin, std::string("http://[::1]:4430"));
    }
    // Origin
    CHECK_EQ(f.request("GET", "/admin/conclusion-publication?id=public", {{"host", "localhost:4430"}, {"origin", "http://localhost:4430"}}).status, 200);
    CHECK_EQ(f.request("GET", "/admin/conclusion-publication?id=public", {{"host", "localhost:80"}, {"origin", "http://localhost"}}).status, 200);
    for (const std::string& origin : std::vector<std::string>{"http://evil.example", "http://localhost", "https://localhost:4430", "http://127.0.0.1:4430", "null"}) {
        auto r = f.request("GET", "/admin/conclusion-publication?id=public", {{"host", "localhost:4430"}, {"origin", origin}});
        CHECK_EQ(r.status, 403);
        CHECK_EQ(error_of(r), std::string("origin_denied"));
    }
    auto ed = f.request("GET", "/admin/conclusion-publication/editor", kLocal);
    CHECK_EQ(ed.status, 200);
    CHECK_EQ(ed.headers.get("cache-control").value_or(""), std::string("private, no-store"));
    CHECK_EQ(f.request("GET", "/admin/conclusion-publication/editor", {{"host", "evil"}}).status, 403);

    const auto parts = api::request_url_parts([] {
        http::Request r;
        r.target = "/x";
        r.headers.set("host", "LocalHost:0080");
        return r;
    }());
    CHECK(parts.has_value());
    CHECK_EQ(parts->hostname, std::string("localhost"));
    CHECK_EQ(parts->origin, std::string("http://localhost"));
}

TEST_CASE("admin PUT: 本文の検査 (content-type / 形 / キー / 型 / id の長さ / 壊れた・巨大)") {
    Fixture f;
    auto put = [&](const std::string& body, const std::string& ct = "application/json") {
        std::vector<std::pair<std::string, std::string>> h = {{"host", "localhost"}, {"origin", "http://localhost"}};
        if (!ct.empty()) h.push_back({"content-type", ct});
        return f.request("PUT", "/admin/conclusion-publication", h, body);
    };
    const std::string rev = integrations::publication_state(*f.db, "public")->revision;
    auto body = [&](const std::string& id, const std::string& extra = "") {
        return "{\"id\":" + json_stringify(Json(id)) + ",\"publishable\":true,\"scope\":\"public\",\"revision\":\"" + rev + "\"" + extra + "}";
    };
    CHECK_EQ(put(body("public"), "text/plain").status, 415);
    CHECK_EQ(put(body("public"), "").status, 415);
    CHECK_EQ(put(body("public"), "Application/JSON").status, 415);  // startsWith は大文字小文字を区別する
    CHECK_EQ(put("{").status, 400);
    CHECK_EQ(put("[]").status, 400);
    CHECK_EQ(put("null").status, 400);
    CHECK_EQ(put(body("public", ",\"role\":\"admin\"")).status, 400);
    CHECK_EQ(put("{\"id\":\"public\",\"publishable\":\"true\",\"scope\":\"public\",\"revision\":\"" + rev + "\"}").status, 400);
    CHECK_EQ(put("{\"id\":\"public\",\"publishable\":true,\"scope\":\"world\",\"revision\":\"" + rev + "\"}").status, 400);
    CHECK_EQ(put("{\"id\":\"public\",\"publishable\":true,\"scope\":\"public\"}").status, 400);
    CHECK_EQ(put(body("")).status, 400);
    CHECK_EQ(put(body(std::string(501, 'a'))).status, 400);       // 上限 + 1
    CHECK_EQ(put(body(std::string(500, 'a'))).status, 409);       // 上限ちょうど (存在しない id)
    std::string jp;
    for (int i = 0; i < 500; ++i) jp += "あ";                      // UTF-16 で 500 (バイトでは 1500) はちょうど
    CHECK_EQ(put(body(jp)).status, 409);
    CHECK_EQ(put(body(std::string(2u << 20, 'a'))).status, 400);  // 巨大
    CHECK_EQ(put(body("public", ",\"x\":" + std::string(5000, '[') + std::string(5000, ']'))).status, 400);
    CHECK_EQ(put(body("public")).status, 200);
    // revision が古ければ 409
    f.db->run("UPDATE conclusion_cache SET conclusion='changed' WHERE gap_id='public'");
    CHECK_EQ(put(body("public")).status, 409);
}

int main() { return di::test::run_all(); }
