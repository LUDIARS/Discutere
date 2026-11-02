// src/crawler/md-format.ts の移植のテスト (Node の出力と同じか: fixtures/crawler/md_format.json)。
#include "check.hpp"
#include "crawler/md_format.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::crawler;

TEST_CASE("parse_game_kg matches Node golden") {
    const Json cases = w1d::load_fixture("crawler/md_format.json");
    CHECK(cases.size() >= 10);
    for (const auto& c : cases) {
        const std::string input = c["input"].get<std::string>();
        if (c["ok"].get<bool>()) {
            auto kg = parse_game_kg(input, std::string("x.md"));
            w1d::check_json_eq(to_json(kg), c["value"], input.c_str());
        } else {
            try {
                (void)parse_game_kg(input, std::string("x.md"));
                throw test::Failure("expected error for: " + input);
            } catch (const GameKGParseError& e) {
                CHECK_EQ(std::string(e.what()), c["error"].get<std::string>());
            }
        }
    }
}

TEST_CASE("frontmatter for stringify omits undefined and empty arrays") {
    auto kg = parse_game_kg("---\nid: a\ntitle: t\nmechanics:\n  - name: m\n    intended_valence: positive\n---\nbody\n");
    const Json fm = game_kg_frontmatter(kg);
    CHECK_EQ(fm.dump(), std::string(R"({"id":"a","title":"t","workspace_id":"knowledge","mechanics":[{"name":"m","intended_valence":"positive"}]})"));
    CHECK_EQ(*kg.body, std::string("body\n"));
}

TEST_CASE("broken and oversized front matter are rejected") {
    CHECK_THROWS(parse_game_kg("---\nid: a\ntitle: &anchor t\n---\n"));
    std::string huge = "---\nid: a\ntitle: t\nx: '" + std::string((1u << 20) + 10, 'a') + "'\n---\n";
    CHECK_THROWS(parse_game_kg(huge));
}

DI_TEST_MAIN()
