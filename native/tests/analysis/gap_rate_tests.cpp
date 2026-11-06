// w2-J: src/analysis/gap-rate.ts の移植を Node の golden (架空のゲーム定義と発話) と照合する。
#include "check.hpp"
#include "core/test_util.hpp"

#include "analysis/gap_rate.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "core/db/schema.hpp"

using namespace di;

namespace {
std::string build_fixture(const test::TempDir& dir, const Json& g) {
    fs::ensure_dir(dir.file("games"));
    CHECK(fs::write_file_atomic(dir.file("games/fic-game.md"), g["md"].get<std::string>()));
    const std::string db_path = dir.file("kg.sqlite");
    auto db = sql::Database::open(db_path);
    core::db::apply_migrations(*db);
    for (const auto& s : g["sessions"])
        db->run("INSERT INTO sessions (id, workspace_id, title, started_at, created_at, updated_at) VALUES (?, 'ws', ?, 1, 1, 1)",
                {s["id"].get<std::string>(), s["title"].get<std::string>()});
    for (const auto& u : g["utterances"])
        db->run("INSERT INTO utterances (id, workspace_id, session_id, raw_content, posted_at, created_at, updated_at) VALUES (?, 'ws', ?, ?, ?, 1, 1)",
                {u["id"].get<std::string>(), u["session_id"].get<std::string>(), u["raw_content"].get<std::string>(), u["posted_at"].get<double>()});
    db->close();
    return db_path;
}
} // namespace

TEST_CASE("analyze_gap_rate / format_gap_report (golden)") {
    const Json g = test::load_fixture("w2j/gap_rate.json");
    test::TempDir dir;
    const auto db_path = build_fixture(dir, g);
    const auto report = analysis::analyze_gap_rate("fic-game", dir.file("games"), db_path);
    CHECK_EQ(json_stringify(analysis::to_json(report)), json_stringify(g["report"]));
    CHECK_EQ(analysis::format_gap_report(report), g["text"].get<std::string>());
    try {
        analysis::analyze_gap_rate("nope", dir.file("games"), db_path);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(str::starts_with(e.what(), "GameKG md が見つかりません: "));
        // 区切りは Windows では \ (Node の path.join と同じ) なので区切りの後ろだけ見る
        CHECK(str::ends_with(e.what(), "nope.md (運営の想定感情を定義してください)"));
    }
}

TEST_CASE("gap-rate の境界: パスの脱出・無い DB・大きすぎる md・発話 0 件") {
    const Json g = test::load_fixture("w2j/gap_rate.json");
    test::TempDir dir;
    const auto db_path = build_fixture(dir, g);
    CHECK_THROWS(analysis::load_intended("../games/fic-game", dir.file("games")));
    CHECK_THROWS(analysis::load_intended("/etc/passwd", dir.file("games")));
    CHECK_THROWS(analysis::load_observed("fic-game", dir.file("missing.sqlite")));
    CHECK(!fs::exists(dir.file("missing.sqlite")));  // 読み取り専用で開き、作らない
    CHECK(analysis::load_observed("no-such-game", db_path).empty());
    CHECK(fs::write_file_atomic(dir.file("games/big.md"), "---\nid: big\ntitle: x\n---\n" + std::string(analysis::kMaxGameKgBytes, 'a')));
    CHECK_THROWS(analysis::load_intended("big", dir.file("games")));
    const auto empty = analysis::analyze_gap_rate("fic-game", dir.file("games"), dir.file("kg.sqlite"));
    CHECK(empty.total > 0);
}

DI_TEST_MAIN()
