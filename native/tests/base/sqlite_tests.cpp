#include "check.hpp"

#include "base/sqlite.hpp"

using namespace di;

TEST_CASE("run/get/all mirror better-sqlite3") {
    auto db = sql::Database::open(":memory:");
    db->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, score REAL, data BLOB)");
    auto r = db->run("INSERT INTO t (name, score) VALUES (?, ?)", {"a", 1.5});
    CHECK_EQ(r.changes, 1);
    CHECK_EQ(r.last_insert_rowid, 1);
    db->run("INSERT INTO t (name, score) VALUES (@name, @score)", sql::Params::named({{"name", "b"}, {"score", nullptr}}));
    auto row = db->get("SELECT * FROM t WHERE name = ?", {"b"});
    CHECK(row.has_value());
    CHECK(row->is_null("score"));
    CHECK_EQ(row->i64("id"), 2);
    CHECK(!db->get("SELECT * FROM t WHERE name = ?", {"zz"}).has_value());
    auto rows = db->all("SELECT name FROM t ORDER BY id");
    CHECK_EQ(rows.size(), std::size_t(2));
    CHECK_EQ(rows[1].str("name"), std::string("b"));
    CHECK_EQ(json_stringify(rows[0].to_json()), std::string("{\"name\":\"a\"}"));
}

TEST_CASE("constraint errors carry better-sqlite3 code names") {
    auto db = sql::Database::open(":memory:");
    db->exec("CREATE TABLE t (name TEXT UNIQUE)");
    db->run("INSERT INTO t VALUES (?)", {"x"});
    try {
        db->run("INSERT INTO t VALUES (?)", {"x"});
        CHECK(false);
    } catch (const sql::Error& e) {
        CHECK(e.is_constraint());
        CHECK_EQ(e.code_name(), std::string("SQLITE_CONSTRAINT_UNIQUE"));
    }
}

TEST_CASE("transaction rolls back on exception and nests with savepoints") {
    auto db = sql::Database::open(":memory:");
    db->exec("CREATE TABLE t (v INTEGER)");
    try {
        db->transaction([&] {
            db->run("INSERT INTO t VALUES (1)");
            throw std::runtime_error("boom");
        });
    } catch (const std::runtime_error&) {
    }
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM t")->i64("n"), 0);
    db->transaction([&] {
        db->run("INSERT INTO t VALUES (1)");
        try {
            db->transaction([&] {
                db->run("INSERT INTO t VALUES (2)");
                throw std::runtime_error("inner");
            });
        } catch (const std::runtime_error&) {
        }
    });
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM t")->i64("n"), 1);
}

TEST_CASE("wrong parameter count and multi statements are rejected") {
    auto db = sql::Database::open(":memory:");
    db->exec("CREATE TABLE t (v INTEGER)");
    CHECK_THROWS(db->run("INSERT INTO t VALUES (?)", {1, 2}));
    CHECK_THROWS(db->run("INSERT INTO t VALUES (1); DROP TABLE t"));
}

TEST_CASE("all() charges rows against the limit (boundary)") {
    auto db = sql::Database::open(":memory:");
    db->exec("CREATE TABLE t (v INTEGER)");
    for (int i = 0; i < 10; ++i) db->run("INSERT INTO t VALUES (?)", {i});
    sql::QueryLimits limits;
    limits.max_rows = 10;
    CHECK_EQ(db->all("SELECT v FROM t", {}, limits).size(), std::size_t(10));
    limits.max_rows = 9;
    CHECK_THROWS(db->all("SELECT v FROM t", {}, limits));
    limits.max_rows = 100;
    limits.max_bytes = 3;
    db->exec("CREATE TABLE s (v TEXT)");
    db->run("INSERT INTO s VALUES ('abcd')");
    CHECK_THROWS(db->all("SELECT v FROM s", {}, limits));
}

TEST_CASE("sqlite version is the one bundled by better-sqlite3 13.0.3") {
    auto db = sql::Database::open(":memory:");
    CHECK_EQ(db->get("SELECT sqlite_version() AS v")->str("v"), std::string("3.53.4"));
    CHECK_EQ(db->pragma_value("foreign_keys")->as_int(), 1);  // SQLITE_DEFAULT_FOREIGN_KEYS=1
}

DI_TEST_MAIN()
