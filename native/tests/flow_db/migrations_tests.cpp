// フロー DB マイグレーション (src/flow/db/migrations.ts / connection.ts) の互換。
// tests/flow/foundation.test.ts の「DB マイグレーション」節と golden (sqlite_master) を移す。
#include "core/test_util.hpp"

#include "base/env.hpp"
#include "flow/db/connection.hpp"
#include "flow/db/migrations.hpp"

using namespace di;
namespace fdb = di::flow::db;

TEST_CASE("flow migrations are the same statements in the same order as TS") {
    const Json fx = test::load_fixture("flow_db/migrations.json");
    const auto& migs = fdb::flow_migrations();
    CHECK_EQ(migs.size(), fx["migrations"].size());
    for (std::size_t i = 0; i < migs.size(); ++i) {
        CHECK_EQ(migs[i].id, fx["migrations"][i]["id"].get<std::string>());
        CHECK_EQ(migs[i].sql.size(), fx["migrations"][i]["sql"].size());
        for (std::size_t k = 0; k < migs[i].sql.size(); ++k) CHECK_EQ(migs[i].sql[k], fx["migrations"][i]["sql"][k].get<std::string>());
    }
}

TEST_CASE("migration on new DB matches Node sqlite_master") {
    const Json fx = test::load_fixture("flow_db/migrations.json");
    test::TempDir dir;
    auto db = sql::Database::open(dir.file("new.db"));
    fdb::apply_flow_migrations(*db);
    CHECK_EQ(json_stringify(test::sqlite_master(*db)), json_stringify(fx["sqlite_master"]));
    auto cols = db->table_columns("discussion_paper");
    CHECK(std::find(cols.begin(), cols.end(), "discussion_no") != cols.end());
    CHECK(db->get("SELECT id FROM _flow_migrations WHERE id = 'flow_0001_initial'").has_value());
}

TEST_CASE("migration idempotent (applying twice is fine)") {
    test::TempDir dir;
    auto db = sql::Database::open(dir.file("existing.db"));
    fdb::apply_flow_migrations(*db);
    fdb::apply_flow_migrations(*db);
    CHECK_EQ(db->get("SELECT COUNT(*) AS c FROM _flow_migrations")->i64("c"),
             static_cast<std::int64_t>(fdb::flow_migrations().size()));
}

TEST_CASE("existing DB: columns already added by hand are ignored (duplicate column)") {
    const Json fx = test::load_fixture("flow_db/migrations.json");
    test::TempDir dir;
    auto db = sql::Database::open(dir.file("partial.db"));
    db->exec("CREATE TABLE IF NOT EXISTS _flow_migrations (id TEXT PRIMARY KEY, applied_at INTEGER NOT NULL)");
    for (std::size_t i = 0; i < 12; ++i) {
        for (const auto& s : fx["migrations"][i]["sql"]) db->exec(s.get<std::string>());
        db->run("INSERT INTO _flow_migrations (id, applied_at) VALUES (?, ?)", {fx["migrations"][i]["id"].get<std::string>(), 1});
    }
    // 次の migration の ALTER を先に手で当てておいても失敗にならない
    const auto& next = fdb::flow_migrations()[12];
    for (const auto& s : next.sql)
        if (s.rfind("ALTER", 0) == 0) db->exec(s);
    fdb::apply_flow_migrations(*db);
    CHECK_EQ(json_stringify(test::sqlite_master(*db)), json_stringify(fx["sqlite_master"]));
}

TEST_CASE("unexpected migration errors throw and are not recorded") {
    test::TempDir dir;
    auto db = sql::Database::open(dir.file("broken.db"));
    db->exec("CREATE VIEW discussion_paper AS SELECT 1 AS id");
    CHECK_THROWS(fdb::apply_flow_migrations(*db));
    CHECK(!db->get("SELECT id FROM _flow_migrations WHERE id = 'flow_0001_initial'").has_value());
    CHECK(!db->in_transaction());
}

TEST_CASE("is_ignorable_migration_error matches only ALTER TABLE + duplicate column") {
    CHECK(fdb::is_ignorable_migration_error("  alter\n table x ADD COLUMN y", "exec: Duplicate column name: y"));
    CHECK(!fdb::is_ignorable_migration_error("CREATE TABLE x (a)", "duplicate column name: a"));
    CHECK(!fdb::is_ignorable_migration_error("ALTER TABLE x ADD COLUMN y", "no such table: x"));
    CHECK(!fdb::is_ignorable_migration_error("ALTERTABLE x", "duplicate column name"));
}

TEST_CASE("get_flow_db opens DATABASE_PATH with WAL and migrations, and shares one instance") {
    test::TempDir dir;
    env::set_for_test("DATABASE_PATH", dir.file("sub/flow.db"));
    fdb::reset_flow_db_for_test();
    auto a = fdb::get_flow_db();
    auto b = fdb::get_flow_db();
    CHECK(a.get() == b.get());
    CHECK_EQ(a->pragma_value("journal_mode")->as_string(), std::string("wal"));
    CHECK(a->table_exists("flow_utterance"));
    fdb::reset_flow_db_for_test();
    env::set_for_test("DATABASE_PATH", std::nullopt);
}

DI_TEST_MAIN()
