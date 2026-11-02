// src/feedback/store.ts の移植テスト。TS: tests/feedback/store.test.ts
#include "../check.hpp"
#include "base/strings.hpp"
#include "feedback/store.hpp"

using namespace di;

TEST_CASE("GameFeedbackStore: 追加・件数・新しい順・匿名性・一覧") {
    auto db = sql::Database::open(":memory:");
    feedback::GameFeedbackStore store(db);
    const std::int64_t t0 = 1'000'000;
    CHECK(store.add("Vampire Survivors", "ビルドの自由度が高くて中毒性ある", t0).has_value());
    CHECK(store.add(" Vampire Survivors ", " 終盤の処理落ちが気になる ", t0 + 10).has_value());
    CHECK(store.add("ワンダと巨像", "孤独感の演出が刺さる", t0 + 20).has_value());
    CHECK(!store.add("X", "   ", t0).has_value());
    CHECK(!store.add("  ", "本文", t0).has_value());
    CHECK_EQ(store.count_for_game("Vampire Survivors"), std::int64_t{2});
    CHECK_EQ(store.count_for_game("ワンダと巨像"), std::int64_t{1});
    const auto recent = store.recent_for_game("Vampire Survivors", 12);
    CHECK_EQ(recent.size(), std::size_t{2});
    CHECK_EQ(recent[0].content, std::string("終盤の処理落ちが気になる"));
    CHECK_EQ(store.recent_for_game("Vampire Survivors", 1).size(), std::size_t{1});
    for (const auto& c : db->table_columns("game_feedback")) {
        const auto l = str::to_lower_ascii(c);
        CHECK(!str::contains(l, "author") && !str::contains(l, "user") && !str::contains(l, "name"));
    }
    const auto games = store.list_games();
    CHECK_EQ(games[0].gameTitle, std::string("Vampire Survivors"));
    CHECK_EQ(games[0].count, std::int64_t{2});
    CHECK_EQ(json_stringify(feedback::to_json(games[0])), std::string(R"({"gameTitle":"Vampire Survivors","count":2})"));
}

DI_TEST_MAIN()
