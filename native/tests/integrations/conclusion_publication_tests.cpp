// src/integrations/conclusion-publication.ts の移植テスト。
// TS: tests/visualize/conclusion-publication.test.ts (状態・公開・内容変更で失効)
// golden: native/tests/golden/w1-A-integrations.gen.ts
#include "../check.hpp"
#include "base/fsutil.hpp"
#include "integrations/conclusion_publication.hpp"

using namespace di;
using namespace di::integrations;

TEST_CASE("golden: publicationState / setPublication / publishedConclusions の操作列") {
    auto text = fs::read_file(test::fixtures_dir() + "/integrations/conclusion_publication.json");
    CHECK(text.has_value());
    const Json fx = parse_json_or_throw(*text, JsonLimits{8u << 20, 1'000'000, 64});
    auto db = sql::Database::open(":memory:");
    for (const auto& s : fx["sql"]) db->exec(s.get<std::string>());
    CHECK(fx["steps"].size() >= 15);
    for (const auto& step : fx["steps"]) {
        const std::string op = step["op"].get<std::string>();
        const Json& a = step["args"];
        Json actual;
        if (op == "state") {
            auto s = publication_state(*db, a["id"].get<std::string>());
            actual = s ? to_json(*s) : Json(nullptr);
        } else if (op == "set") {
            actual = set_publication(*db, a["id"].get<std::string>(), a["publishable"].get<bool>(), a["scope"].get<std::string>(),
                                     a["revision"].get<std::string>(), a["now"].get<std::int64_t>());
        } else if (op == "published") {
            std::optional<std::string> id;
            if (a.contains("id")) id = a["id"].get<std::string>();
            actual = Json::array();
            for (const auto& c : published_conclusions(*db, id)) actual.push_back(to_json(c));
        } else if (op == "exec") {
            actual = db->run(a["sql"].get<std::string>()).changes;
        }
        if (json_stringify(actual) != json_stringify(step["result"])) std::cerr << "step " << json_stringify(step) << "\n";
        CHECK_EQ(json_stringify(actual), json_stringify(step["result"]));
    }
}

TEST_CASE("公開は 200 件まで") {
    auto db = sql::Database::open(":memory:");
    db->exec("CREATE TABLE conclusion_cache (gap_id TEXT PRIMARY KEY, kind TEXT NOT NULL, session_id TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', conclusion TEXT, updated_at INTEGER NOT NULL, cached_at INTEGER NOT NULL)");
    db->exec("CREATE TABLE conclusion_publication (conclusion_id TEXT PRIMARY KEY, publishable INTEGER NOT NULL DEFAULT 0, scope TEXT NOT NULL DEFAULT 'private', content_hash TEXT NOT NULL, updated_at INTEGER NOT NULL)");
    for (int i = 0; i < 205; ++i) {
        const std::string id = "c" + std::to_string(i);
        db->run("INSERT INTO conclusion_cache VALUES (?, 'gap', 's', 't', 'r', ?, 0)", {id, i});
        CHECK(set_publication(*db, id, true, "public", publication_state(*db, id)->revision, 1));
    }
    CHECK_EQ(published_conclusions(*db).size(), std::size_t{200});
}

DI_TEST_MAIN()
