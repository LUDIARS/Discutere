// tests/discord-hook/{facilitator-directives,reactions}.test.ts の移植 (一時ディレクトリの SQLite)。
#include "base/strings.hpp"
#include "check.hpp"
#include "discord_hook/facilitator_directives.hpp"
#include "discord_hook/reactions.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::discord_hook;

TEST_CASE("facilitator directives store and scene resolution") {
    const std::string dir = w1d::temp_dir("directives");
    auto db = sql::Database::open(dir + "/kg.sqlite");
    // resolveActiveGapForScene が読む core の表 (列は使う分だけ)
    db->exec("CREATE TABLE sessions (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, scene TEXT, started_at INTEGER);"
             "CREATE TABLE design_gaps (id TEXT PRIMARY KEY, workspace_id TEXT, status TEXT);");
    ensure_facilitator_directive_table(*db);
    ensure_facilitator_directive_table(*db);  // 冪等
    const std::string ws = "knowledge";
    CHECK(add_facilitator_directive(*db, {ws, "gap-x", "もっと簡単な言葉で", std::string("u1")}).has_value());
    CHECK(add_facilitator_directive(*db, {ws, "gap-x", "もっと否定的に", std::string("u2")}).has_value());
    CHECK(!add_facilitator_directive(*db, {ws, "gap-x", "   ", std::nullopt}).has_value());
    CHECK_EQ(list_facilitator_directives(*db, ws, "gap-x"), (std::vector<std::string>{"もっと簡単な言葉で", "もっと否定的に"}));
    CHECK(list_facilitator_directives(*db, ws, "gap-other").empty());
    for (int i = 0; i < 8; ++i) add_facilitator_directive(*db, {ws, "gap-y", "d" + std::to_string(i), std::nullopt});
    const auto latest = list_facilitator_directives(*db, ws, "gap-y");
    CHECK_EQ(latest, (std::vector<std::string>{"d3", "d4", "d5", "d6", "d7"}));  // 直近 5 件を時系列で
    CHECK_EQ(list_facilitator_directives(*db, ws, "gap-y", 0).size(), std::size_t(1));
    // author_id は保存されるが一覧 (prompt) には出ない
    auto row = db->get("SELECT author_id FROM facilitator_directives WHERE text = ?", {"もっと否定的に"});
    CHECK_EQ(row->str("author_id"), std::string("u2"));
    // 長文は上限で切る
    add_facilitator_directive(*db, {ws, "gap-z", std::string(1000, 'x'), std::nullopt});
    CHECK_EQ(str::js_length(list_facilitator_directives(*db, ws, "gap-z")[0]), DIRECTIVE_TEXT_CAP);

    db->run("INSERT INTO design_gaps VALUES ('g1', ?, 'open')", {ws});
    db->run("INSERT INTO sessions VALUES ('s1', ?, 'discussion-of-gap:g1', 'discord:g1/ch1', 1)", {ws});
    CHECK_EQ(*resolve_active_gap_for_scene(*db, ws, "discord:g1/ch1"), std::string("g1"));
    CHECK(!resolve_active_gap_for_scene(*db, ws, "discord:g9/ch9"));
    db->run("UPDATE design_gaps SET status = 'closed' WHERE id = 'g1'");
    CHECK(!resolve_active_gap_for_scene(*db, ws, "discord:g1/ch1"));
}

TEST_CASE("reaction scoring") {
    CHECK_EQ(reaction_weight("👍"), 1);
    CHECK_EQ(reaction_weight("💡"), 3);
    CHECK_EQ(reaction_weight("🤔"), 1);
    CHECK_EQ(reaction_weight("👍", ReactionWeights{{"👍", 5}}), 5);
    auto db = sql::Database::open(":memory:");
    ensure_reaction_tables(*db);
    record_posted_message(*db, {"m1", "u1", "utterance", std::nullopt, std::nullopt});
    record_posted_message(*db, {"m2", "u2", "utterance", std::string("c"), 5});
    record_posted_message(*db, {"", "u9", "utterance", std::nullopt, std::nullopt});
    CHECK(!apply_reaction(*db, {"unknown", "👍", std::nullopt, std::nullopt}));
    auto r1 = apply_reaction(*db, {"m1", "👍", std::nullopt, std::nullopt});
    CHECK_EQ(r1->targetId, std::string("u1"));
    CHECK_EQ(r1->score, std::int64_t(1));
    auto r2 = apply_reaction(*db, {"m1", "💡", std::nullopt, std::nullopt});
    CHECK_EQ(r2->score, std::int64_t(4));
    apply_reaction(*db, {"m2", "🔥", std::nullopt, std::nullopt});
    CHECK_EQ(get_opinion_score(*db, "u2"), std::int64_t(2));
    CHECK_EQ(get_opinion_score(*db, "u3"), std::int64_t(0));
    auto top = top_scored_opinions(*db, {"u1", "u2", "u3"}, 5);
    CHECK_EQ(top.size(), std::size_t(2));
    CHECK_EQ(top[0].targetId, std::string("u1"));
    CHECK_EQ(top_scored_opinions(*db, {"u1", "u2"}, 1).size(), std::size_t(1));
}

DI_TEST_MAIN()
