// KG 共有同期 (src/kg-sync/*)。tests/kg-sync/run.ts の観点 (round-trip / 共有フラグ / LWW / whitelist) と
// golden (Node の export NDJSON・import 結果・取り込み後の行)、上限と状態ファイル。
#include "core/test_util.hpp"

#include <stdexcept>

#include "core/db/schema.hpp"
#include "kg_sync/export.hpp"
#include "kg_sync/import.hpp"
#include "kg_sync/shared_tables.hpp"
#include "kg_sync/state.hpp"

using namespace di;
using namespace di::kg_sync;

namespace {
sql::DatabasePtr fresh_db() {
    auto db = sql::Database::open(":memory:");
    core::db::apply_migrations(*db);
    return db;
}
void insert_game(sql::Database& db, const std::string& id, const std::string& title, double updated, double shared) {
    db.run("INSERT INTO games (id, workspace_id, title, genre, created_at, updated_at, shared) VALUES (?, ?, ?, ?, ?, ?, ?)",
           {id, "knowledge", title, nullptr, updated, updated, shared});
}
std::string row_line(const Json& data, const std::string& table = "games") {
    return json_stringify(Json{{"type", "row"}, {"table", table}, {"data", data}});
}
} // namespace

TEST_CASE("TS scenario: round-trip, shared flag, increments, LWW, whitelist") {
    auto master = fresh_db();
    insert_game(*master, "g-shared-1", "Vampire Survivors", 1000, 1);
    insert_game(*master, "g-shared-2", "Hades", 2000, 1);
    insert_game(*master, "g-private", "社内メモ", 3000, 0);
    master->run("INSERT INTO mechanics (id, workspace_id, game_id, name, description, created_at, updated_at, shared) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                {"m-shared-1", "knowledge", "g-shared-1", "経験値磁石", nullptr, 1500.0, 1500.0, 1.0});
    KgExportOptions opts;
    opts.since = 0;
    opts.workspaceId = "knowledge";
    auto full = export_shared_kg(*master, opts);
    CHECK_EQ(full.count, 3u);
    CHECK_EQ(full.watermark, 2000.0);
    CHECK(full.ndjson.find("g-private") == std::string::npos);

    auto follower = fresh_db();
    auto imp = import_shared_kg(*follower, full.ndjson);
    CHECK_EQ(imp.applied, 3u);
    CHECK_EQ(follower->all("SELECT id FROM games").size(), 2u);
    CHECK_EQ(follower->get("SELECT title FROM games WHERE id=?", {"g-shared-2"})->str("title"), std::string("Hades"));

    opts.since = full.watermark;
    CHECK_EQ(export_shared_kg(*master, opts).count, 0u);

    insert_game(*follower, "g-lww", "follower-new", 5000, 1);
    auto old = import_shared_kg(*follower, row_line({{"id", "g-lww"}, {"workspace_id", "knowledge"}, {"title", "master-old"},
                                                     {"genre", nullptr}, {"created_at", 100}, {"updated_at", 100}, {"shared", 1}}));
    CHECK(old.skipped == 1 && old.applied == 0);
    CHECK_EQ(follower->get("SELECT title FROM games WHERE id='g-lww'")->str("title"), std::string("follower-new"));
    import_shared_kg(*follower, row_line({{"id", "g-lww"}, {"workspace_id", "knowledge"}, {"title", "master-new"},
                                          {"genre", nullptr}, {"created_at", 100}, {"updated_at", 9000}, {"shared", 1}}));
    CHECK_EQ(follower->get("SELECT title FROM games WHERE id='g-lww'")->str("title"), std::string("master-new"));

    auto forged = import_shared_kg(*follower, row_line({{"id", "s-evil"}, {"workspace_id", "knowledge"}, {"title", "x"},
                                                        {"started_at", 1}, {"created_at", 1}, {"updated_at", 1}},
                                                       "sessions"));
    CHECK(forged.ignored == 1 && forged.applied == 0);
    CHECK_EQ(follower->get("SELECT COUNT(*) c FROM sessions")->i64("c"), 0);
}

TEST_CASE("golden: export NDJSON, import results and follower rows match Node") {
    const Json fx = test::load_fixture("kg_sync/kg_sync.json");
    auto db = fresh_db();
    const char* ins = "INSERT INTO games (id, workspace_id, title, genre, created_at, updated_at, shared) VALUES (?, ?, ?, ?, ?, ?, ?)";
    db->run(ins, {"g1", "knowledge", "Vampire \"Survivors\"\n改行", nullptr, 1000.0, 1000.0, 1.0});
    db->run(ins, {"g2", "knowledge", "Hades", "rogue", 2000.0, 2500.0, 1.0});
    db->run(ins, {"g3", "other", "Other", nullptr, 3000.0, 3000.0, 1.0});
    db->run(ins, {"g4", "knowledge", "private", nullptr, 3000.0, 4000.0, 0.0});
    db->run("INSERT INTO mechanics (id, workspace_id, game_id, name, description, created_at, updated_at, shared, intends) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            {"m1", "knowledge", "g1", "経験値磁石", "<desc>", 1500.0, 1500.0, 1.0, "x"});
    db->run("INSERT INTO affects (id, workspace_id, mood, score, created_at, updated_at, shared, valence) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            {"af1", "knowledge", "joy", 0.1 + 0.2, 10.0, 3500.0, 1.0, "pos"});
    db->run("INSERT INTO aesthetics (id, workspace_id, name, created_at, updated_at, shared) VALUES (?, ?, ?, ?, ?, ?)",
            {"ae1", "knowledge", "美", 1.0, 1e15, 1.0});
    for (const auto& c : fx["exports"]) {
        KgExportOptions o;
        if (c["opts"].contains("since")) o.since = c["opts"]["since"].get<double>();
        if (c["opts"].contains("workspaceId")) o.workspaceId = c["opts"]["workspaceId"].get<std::string>();
        const auto r = export_shared_kg(*db, o);
        CHECK_EQ(r.ndjson, c["ndjson"].get<std::string>());
        CHECK_EQ(r.watermark, c["watermark"].get<double>());
        CHECK_EQ(r.count, c["count"].get<std::size_t>());
    }
    auto follower = fresh_db();
    for (const auto& c : fx["imports"]) {
        const std::string nd = c["ndjson"].get<std::string>();
        if (c.contains("error")) {
            CHECK_THROWS(import_shared_kg(*follower, nd));
            continue;
        }
        const auto r = import_shared_kg(*follower, nd);
        CHECK_EQ(r.applied, c["result"]["applied"].get<std::size_t>());
        CHECK_EQ(r.skipped, c["result"]["skipped"].get<std::size_t>());
        CHECK_EQ(r.ignored, c["result"]["ignored"].get<std::size_t>());
        CHECK_EQ(r.watermark, c["result"]["watermark"].get<double>());
    }
    for (auto it = fx["follower"].begin(); it != fx["follower"].end(); ++it)
        CHECK_EQ(json_stringify(test::rows_json(*follower, "SELECT * FROM " + it.key() + " ORDER BY id")), json_stringify(it.value()));
}

TEST_CASE("import limits: exact limit, +1, broken lines, huge lines") {
    auto db = fresh_db();
    const std::string line = row_line({{"id", "g"}, {"workspace_id", "k"}, {"title", "t"}, {"created_at", 1}, {"updated_at", 2}, {"shared", 1}});
    KgImportLimits l;
    l.max_bytes = line.size();
    CHECK_EQ(import_shared_kg(*db, line, l).applied, 1u);         // ちょうど
    CHECK_THROWS(import_shared_kg(*db, line + "\n", l));          // +1 バイト
    l = {};
    l.max_lines = 2;
    CHECK_EQ(import_shared_kg(*db, line + "\n" + line, l).applied, 2u);  // 2 行ちょうど (同じ updated_at は >= で上書き)
    CHECK_THROWS(import_shared_kg(*db, line + "\n" + line + "\n", l));    // 3 行
    l = {};
    l.max_line_bytes = 64;
    const auto r = import_shared_kg(*db, "{\"broken\"\n" + line, l);   // 壊れた行と長すぎる行は ignored
    CHECK_EQ(r.ignored, 2u);
    std::string deep;
    for (int i = 0; i < 100; ++i) deep += "[";
    for (int i = 0; i < 100; ++i) deep += "]";
    CHECK_EQ(import_shared_kg(*db, deep).ignored, 1u);
    // object の値は better-sqlite3 と同じく bind できず、全体が巻き戻る
    const std::string bad = row_line({{"id", "z"}, {"workspace_id", "k"}, {"title", Json::object()}, {"created_at", 1}, {"updated_at", 2}, {"shared", 1}});
    const std::string ok2 = row_line({{"id", "y"}, {"workspace_id", "k"}, {"title", "t"}, {"created_at", 1}, {"updated_at", 2}, {"shared", 1}});
    CHECK_THROWS(import_shared_kg(*db, ok2 + "\n" + bad));
    CHECK(!db->get("SELECT id FROM games WHERE id='y'").has_value());
    CHECK(!db->in_transaction());
}

TEST_CASE("table_columns and whitelist") {
    auto db = fresh_db();
    CHECK(is_shared_table("games"));
    CHECK(!is_shared_table("sessions"));
    CHECK_EQ(table_columns(*db, "games").front(), std::string("id"));
    CHECK_THROWS(table_columns(*db, "nope\"; DROP TABLE games; --"));
    CHECK(db->table_exists("games"));
}

TEST_CASE("sync state file: missing, broken, round-trip, oversized") {
    test::TempDir dir;
    const std::string path = dir.file("state/kg-sync-state.json");
    auto empty = read_sync_state(path);
    CHECK(empty.sources.empty());
    set_source_state(empty, "https://m/kg", {2500, 1700000000000, 3});
    write_sync_state(empty, path);
    CHECK_EQ(*fs::read_file(path),
             std::string("{\n  \"sources\": {\n    \"https://m/kg\": {\n      \"watermark\": 2500,\n      \"lastPulledAt\": 1700000000000,\n      \"lastApplied\": 3\n    }\n  }\n}"));
    auto back = read_sync_state(path);
    CHECK_EQ(get_source_watermark(back, "https://m/kg"), 2500.0);
    CHECK_EQ(get_source_watermark(back, "https://other"), 0.0);
    fs::write_file_atomic(path, "{broken");
    CHECK(read_sync_state(path).sources.empty());
    fs::write_file_atomic(path, std::string(kMaxSyncStateBytes + 1, ' '));
    CHECK(read_sync_state(path).sources.empty());
}

DI_TEST_MAIN()
