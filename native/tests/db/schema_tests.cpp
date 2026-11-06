// サイドカー SQLite スキーマ (src/db/schema.ts の drizzle 定義) の互換。
// golden: drizzle-kit が schema.ts から作る DDL と、それで作った DB の sqlite_master。
#include "core/test_util.hpp"

#include "base/env.hpp"
#include "db/connection.hpp"
#include "db/schema.hpp"

using namespace di;

TEST_CASE("DDL is exactly what drizzle-kit generates") {
    const Json fx = test::load_fixture("db/schema.json");
    std::vector<std::string> flat;
    for (const auto& t : di::db::schema_ddl())
        for (const auto& s : t.statements) flat.push_back(s);
    CHECK_EQ(flat.size(), fx["statements"].size());
    for (std::size_t i = 0; i < flat.size(); ++i) CHECK_EQ(flat[i], fx["statements"][i].get<std::string>());
}

TEST_CASE("ensure_schema builds the same sqlite_master and is idempotent") {
    const Json fx = test::load_fixture("db/schema.json");
    test::TempDir dir;
    auto db = sql::Database::open(dir.file("main.db"));
    CHECK_EQ(di::db::ensure_schema(*db).size(), 9u);
    CHECK_EQ(json_stringify(test::sqlite_master(*db)), json_stringify(fx["sqlite_master"]));
    CHECK(di::db::ensure_schema(*db).empty());
    CHECK_EQ(json_stringify(test::sqlite_master(*db)), json_stringify(fx["sqlite_master"]));
}

TEST_CASE("existing tables are left untouched; only missing tables are created") {
    test::TempDir dir;
    auto db = sql::Database::open(dir.file("old.db"));
    db->exec("CREATE TABLE users (id TEXT PRIMARY KEY, legacy TEXT)");
    auto created = di::db::ensure_schema(*db);
    CHECK_EQ(created.size(), 8u);
    CHECK_EQ(db->table_columns("users").size(), 2u);
}

TEST_CASE("row structs read drizzle columns") {
    test::TempDir dir;
    auto db = sql::Database::open(dir.file("rows.db"));
    di::db::ensure_schema(*db);
    db->run("INSERT INTO channel_monitors (id, workspace_id, platform, channel_id, channel_name, created_by, created_at, updated_at) "
            "VALUES ('m', 'w', 'discord', 'c', 'n', 'u', 100, 200)");
    auto row = db->get(std::string("SELECT * FROM ") + di::db::ChannelMonitor::TABLE);
    CHECK(row.has_value());
    const auto m = di::db::ChannelMonitor::from_row(*row);
    CHECK_EQ(m.captureMessages, true);  // DEFAULT true
    CHECK_EQ(m.mode, std::string("task"));
    CHECK_EQ(m.discussionDelayMinutes, 5);
    CHECK(!m.botToken.has_value());
    CHECK_EQ(m.updatedAt, 200);
}

TEST_CASE("get_main_db opens DATABASE_PATH with WAL + foreign keys") {
    test::TempDir dir;
    env::set_for_test("DATABASE_PATH", dir.file("d/discutere.db"));
    di::db::reset_main_db_for_test();
    auto db = di::db::get_main_db();
    CHECK_EQ(db->pragma_value("journal_mode")->as_string(), std::string("wal"));
    CHECK_EQ(db->pragma_value("foreign_keys")->as_int(), 1);
    CHECK(db->table_exists("ludus_economy_graphs"));
    di::db::reset_main_db_for_test();
    env::set_for_test("DATABASE_PATH", std::nullopt);
}

DI_TEST_MAIN()
