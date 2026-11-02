// ゲーム名の別名展開 (tests/persona-engine/game-aliases.test.ts) + golden + Ludus 用語辞書の中身。
#include "core/test_util.hpp"

#include <algorithm>

#include "discatier_engine_adapter/game_aliases.hpp"
#include "discatier_engine_adapter/ludus_term_aliases.hpp"

using namespace di;
using namespace di::discatier_engine_adapter;

namespace {
using Groups = std::vector<std::vector<std::string>>;
Groups groups_of(const Json& j) {
    Groups out;
    for (const auto& g : j) {
        std::vector<std::string> v;
        for (const auto& s : g) v.push_back(s.get<std::string>());
        out.push_back(v);
    }
    return out;
}
bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
} // namespace

TEST_CASE("TS assertions") {
    CHECK(parse_title_aliases("Genshin Impact (原神)") == std::vector<std::string>({"Genshin Impact", "原神"}));
    CHECK(parse_title_aliases("モンスターストライク") == std::vector<std::string>({"モンスターストライク"}));
    auto groups = build_alias_groups({"Genshin Impact (原神)", "モンスターストライク"});
    CHECK(has(expand_aliases({"モンスターストライク"}, groups), "モンスト"));
    CHECK(has(expand_aliases({"モンスト"}, groups), "モンスターストライク"));
    CHECK(has(expand_aliases({"原神"}, groups), "genshin"));
    CHECK(expand_aliases({"無関係なテーマ"}, groups) == std::vector<std::string>({"無関係なテーマ"}));
    CHECK(has(expand_aliases({"モンスト", "周回"}, groups), "周回"));
    auto extra = build_alias_groups({"モンスターストライク"}, {{"経験値", "XP", "EXP"}, {"単独語のみ"}});
    auto xp = expand_aliases({"経験値"}, extra);
    CHECK(has(xp, "xp") && has(xp, "exp"));
    CHECK(expand_aliases({"単独語のみ"}, extra) == std::vector<std::string>({"単独語のみ"}));
}

TEST_CASE("golden: static groups, Ludus groups, parse, build and expand match Node") {
    const Json fx = test::load_fixture("discatier_engine_adapter/game_aliases.json");
    CHECK(static_alias_groups() == groups_of(fx["staticGroups"]));
    CHECK(ludus_term_alias_groups() == groups_of(fx["ludusGroups"]));
    for (const auto& c : fx["parse"]) CHECK_EQ(Json(parse_title_aliases(c["title"].get<std::string>())), c["out"]);
    std::vector<std::string> titles;
    for (const auto& t : fx["titles"]) titles.push_back(t.get<std::string>());
    const auto groups = build_alias_groups(titles);
    const auto groups_extra = build_alias_groups(titles, ludus_term_alias_groups());
    const auto extra_only = build_alias_groups({"モンスターストライク"}, {{"経験値", "XP", "EXP"}, {"単独語のみ"}, {"a", "bb", "BB", " cc "}});
    CHECK_EQ(Json(groups), fx["groups"]);
    CHECK_EQ(Json(groups_extra), fx["groupsExtra"]);
    CHECK_EQ(Json(extra_only), fx["extraOnly"]);
    for (const auto& c : fx["expand"]) {
        std::vector<std::string> terms;
        for (const auto& t : c["terms"]) terms.push_back(t.get<std::string>());
        const std::string which = c["which"].get<std::string>();
        const Groups& g = which == "groups" ? groups : which == "groupsExtra" ? groups_extra : extra_only;
        CHECK_EQ(Json(expand_aliases(terms, g, c["max"].get<std::size_t>())), c["out"]);
    }
}

TEST_CASE("boundaries: huge title and many terms") {
    std::string big(1u << 20, 'x');
    big += "(";
    CHECK_EQ(parse_title_aliases(big).size(), 1u);
    std::vector<std::string> many(5000, "モンスト");
    CHECK(expand_aliases(many, build_alias_groups({}), 12).size() <= 12);
}

DI_TEST_MAIN()
