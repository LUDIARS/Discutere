// core の SQLite スキーマ (src/core/db/schema.ts / kuzu-client.ts) の互換。
// golden: Node の applyMigrations で作った DB の sqlite_master と、C++ で作った DB の sqlite_master が一致する。
#include "core/test_util.hpp"

#include "base/env.hpp"
#include "core/db/kuzu_client.hpp"
#include "core/db/schema.hpp"

using namespace di;
namespace cdb = di::core::db;

TEST_CASE("migrations are the same statements in the same order as TS") {
    const Json fx = test::load_fixture("core/schema.json");
    const auto& migs = cdb::core_migrations();
    CHECK_EQ(migs.size(), fx["migrations"].size());
    for (std::size_t i = 0; i < migs.size(); ++i) {
        CHECK_EQ(migs[i].id, fx["migrations"][i]["id"].get<std::string>());
        CHECK_EQ(migs[i].sql.size(), fx["migrations"][i]["sql"].size());
        for (std::size_t k = 0; k < migs[i].sql.size(); ++k) CHECK_EQ(migs[i].sql[k], fx["migrations"][i]["sql"][k].get<std::string>());
    }
}

TEST_CASE("KuzuClient creates the same sqlite_master as Node, and reopening is a no-op") {
    const Json fx = test::load_fixture("core/schema.json");
    test::TempDir dir;
    const std::string path = dir.file("nested/dir/discatier.kuzu");
    {
        cdb::KuzuClient client(path);
        CHECK_EQ(json_stringify(test::sqlite_master(client.raw())), json_stringify(fx["sqlite_master"]));
        CHECK_EQ(client.raw().pragma_value("journal_mode")->as_string(), std::string("wal"));
        CHECK_EQ(client.raw().pragma_value("foreign_keys")->as_int(), 1);
        client.close();
    }
    {
        cdb::KuzuClient again(path);
        CHECK_EQ(json_stringify(test::sqlite_master(again.raw())), json_stringify(fx["sqlite_master"]));
        CHECK_EQ(again.raw().get("SELECT COUNT(*) AS c FROM _migrations")->i64("c"), 6);
    }
}

TEST_CASE("existing DB with only early migrations gets the rest added") {
    const Json fx = test::load_fixture("core/schema.json");
    test::TempDir dir;
    const std::string path = dir.file("old.db");
    {
        auto db = sql::Database::open(path);
        db->exec("CREATE TABLE IF NOT EXISTS _migrations (id TEXT PRIMARY KEY, applied_at INTEGER NOT NULL)");
        for (std::size_t i = 0; i < 2; ++i) {
            for (const auto& s : fx["migrations"][i]["sql"]) db->exec(s.get<std::string>());
            db->run("INSERT INTO _migrations (id, applied_at) VALUES (?, ?)", {fx["migrations"][i]["id"].get<std::string>(), 1});
        }
        db->run("INSERT INTO people (id, workspace_id, name, created_at, updated_at) VALUES ('p', 'w', 'n', 1, 1)");
    }
    cdb::KuzuClient client(path);
    CHECK_EQ(json_stringify(test::sqlite_master(client.raw())), json_stringify(fx["sqlite_master"]));
    CHECK_EQ(client.raw().get("SELECT name FROM people")->str("name"), std::string("n"));
}

TEST_CASE("default path comes from DISCATIER_KUZU_PATH") {
    env::set_for_test("DISCATIER_KUZU_PATH", std::string("/tmp/x.kuzu"));
    CHECK_EQ(cdb::default_kuzu_path(), std::string("/tmp/x.kuzu"));
    env::set_for_test("DISCATIER_KUZU_PATH", std::string(""));
    CHECK_EQ(cdb::default_kuzu_path(), std::string("./data/discatier.kuzu"));
    env::set_for_test("DISCATIER_KUZU_PATH", std::nullopt);
    CHECK_EQ(cdb::default_kuzu_path(), std::string("./data/discatier.kuzu"));
}

DI_TEST_MAIN()
