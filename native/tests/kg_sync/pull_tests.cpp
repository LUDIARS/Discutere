// w2-J: src/kg-sync/pull.ts の移植。偽の master (http::Server) から NDJSON を取る。
#include <atomic>

#include "check.hpp"
#include "core/test_util.hpp"

#include "base/strings.hpp"
#include "core/db/schema.hpp"
#include "infra/http_server.hpp"
#include "kg_sync/pull.hpp"

using namespace di;
using namespace di::kg_sync;

namespace {
struct FakeMaster {
    std::unique_ptr<http::Server> server;
    std::string body;
    int status = 200;
    std::string last_target;
    std::string last_header;
    std::atomic<int> hits{0};
    FakeMaster() {
        auto router = std::make_shared<http::Router>();
        router->get("/kg", [this](http::Request& r) {
            ++hits;
            last_target = r.target;
            last_header = r.header("x-kg-token").value_or("");
            return http::Response::bytes(body, "application/x-ndjson", status);
        });
        router->get("/redirect", [](http::Request&) { return http::Response::redirect("http://example.invalid/kg", 302); });
        http::ServerOptions o;
        o.port = 0;
        server = std::make_unique<http::Server>(o, router);
        server->start();
    }
    ~FakeMaster() { server->stop(); }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(server->bound_port()) + path; }
};

sql::DatabasePtr core_db(const test::TempDir& dir) {
    auto db = sql::Database::open(dir.file("kg.sqlite"));
    core::db::apply_migrations(*db);
    return db;
}

std::string game_line(const std::string& id, double updated) {
    return json_stringify(Json{{"type", "row"}, {"table", "games"}, {"data", Json{{"id", id}, {"workspace_id", "ws"}, {"title", "T"}, {"created_at", 1}, {"updated_at", updated}}}}) + "\n";
}
} // namespace

TEST_CASE("build_pull_url: since / token を URLSearchParams#set と同じく付ける") {
    CHECK_EQ(build_pull_url("http://h:8/kg?a=1&since=3&since=4", 10, std::string("t k")), std::string("http://h:8/kg?a=1&since=10&token=t+k"));
    CHECK_EQ(build_pull_url("https://h", 0, std::nullopt), std::string("https://h/?since=0"));
    CHECK_THROWS(build_pull_url("ftp://h/kg", 0, std::nullopt));
    CHECK_THROWS(build_pull_url("http://user:pw@h/kg", 0, std::nullopt));
    CHECK_THROWS(build_pull_url("not a url", 0, std::nullopt));
}

TEST_CASE("pull: 取り込み・透かし・token・失敗の文言に URL/token を出さない") {
    test::TempDir dir;
    FakeMaster master;
    auto db = core_db(dir);
    master.body = game_line("g1", 100) + game_line("g2", 250);
    KgPullOptions o;
    o.url = master.url("/kg");
    o.statePath = dir.file("state.json");
    o.token = "kg-secret-token-fake";
    auto r = pull_shared_kg(*db, o);
    CHECK(r.applied == 2 && r.fromWatermark == 0 && r.toWatermark == 250);
    CHECK(str::contains(master.last_target, "since=0") && str::contains(master.last_target, "token=kg-secret-token-fake"));
    CHECK_EQ(master.last_header, std::string("kg-secret-token-fake"));
    r = pull_shared_kg(*db, o);
    CHECK(str::contains(master.last_target, "since=250"));
    CHECK(r.fromWatermark == 250 && r.applied + r.skipped == 2);
    o.full = true;
    pull_shared_kg(*db, o);
    CHECK(str::contains(master.last_target, "since=0"));

    master.status = 503;
    try {
        pull_shared_kg(*db, o);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("KG pull failed: HTTP 503 Service Unavailable"));
    }
    // リダイレクトは追わない
    o.url = master.url("/redirect");
    try {
        pull_shared_kg(*db, o);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(str::contains(e.what(), "HTTP 302") && !str::contains(e.what(), "kg-secret"));
    }
    // 繋がらない
    o.url = "http://127.0.0.1:1/kg";
    try {
        pull_shared_kg(*db, o);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(str::starts_with(e.what(), "KG pull failed: ") && !str::contains(e.what(), "kg-secret") && !str::contains(e.what(), "127.0.0.1"));
    }
}

TEST_CASE("pull: 応答の上限ちょうど/+1") {
    test::TempDir dir;
    FakeMaster master;
    auto db = core_db(dir);
    master.status = 200;
    master.body = game_line("g1", 1);
    KgPullOptions o;
    o.url = master.url("/kg");
    o.statePath = dir.file("s.json");
    o.limits.max_bytes = master.body.size();
    CHECK_EQ(pull_shared_kg(*db, o).applied, 1.0);
    o.limits.max_bytes = master.body.size() - 1;
    try {
        pull_shared_kg(*db, o);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK_EQ(std::string(e.what()), std::string("KG pull failed: response too large"));
    }
}

DI_TEST_MAIN()
