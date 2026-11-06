// tests/persona-engine/repos.test.ts + session-fire-store.test.ts の移植 (一時ファイルの SQLite)。
#include "check.hpp"

#include <filesystem>

#include "base/crypto.hpp"
#include "base/fsutil.hpp"
#include "base/time.hpp"
#include "persona_engine/db/migrations.hpp"
#include "persona_engine/db/personas_repo.hpp"
#include "persona_engine/db/rules_repo.hpp"
#include "persona_engine/db/session_fires_repo.hpp"
#include "persona_engine/seeds/personas.hpp"
#include "persona_engine/seeds/rules.hpp"
#include "persona_engine/worker_pool/debate_rules.hpp"

using namespace di;
using namespace di::persona_engine;
using namespace di::persona_engine::db;

namespace {
std::string tmp_db() {
    const auto dir = std::filesystem::temp_directory_path() / ("pe-repos-" + crypto::uuid_v4());
    fs::ensure_dir(fs::from_path(dir));
    return fs::from_path(dir / "pe.db");
}
} // namespace

TEST_CASE("repos: migrations idempotent / seeds / assignments / learned_notes / rules / logs / reconcile") {
    auto db = sql::Database::open(tmp_db());
    apply_persona_engine_migrations(*db);
    apply_persona_engine_migrations(*db);  // 冪等
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM _pe_migrations")->i64("n"), std::int64_t(2));

    PersonasRepo personas(db);
    RulesRepo rules(db);
    personas.bulk_seed(seeds::DISCUSSION_PERSONA_SEEDS());
    personas.bulk_seed(seeds::DISCUSSION_PERSONA_SEEDS());  // INSERT OR IGNORE
    CHECK_EQ(personas.list().size(), seeds::DISCUSSION_PERSONA_SEEDS().size());
    auto advocate = personas.get("advocate");
    CHECK(advocate.has_value());
    CHECK_EQ(advocate->name, std::string("推進派"));
    CHECK_EQ(advocate->display_name, std::string("押野 進"));
    CHECK_EQ(advocate->traits, std::string(R"(["前のめり","速度重視","仮説量産","細部より方向","肯定的"])"));
    CHECK_EQ(advocate->learned_notes, std::string("[]"));
    CHECK(!personas.get("nope"));

    auto a = personas.assign("advocate", "session-1");
    CHECK_EQ(a.persona_id, std::string("advocate"));
    CHECK(!a.released_at);
    CHECK_EQ(personas.active_assignments_for_session("session-1").size(), std::size_t(1));
    personas.release(a.id);
    CHECK(personas.active_assignments_for_session("session-1").empty());

    personas.append_learned_note("advocate", "first lesson");
    const auto notes = parse_json_or_throw(personas.get("advocate")->learned_notes);
    CHECK_EQ(notes.size(), std::size_t(1));
    CHECK_EQ(notes[0]["note"].get<std::string>(), std::string("first lesson"));
    CHECK_THROWS(personas.append_learned_note("missing", "x"));

    auto fb = personas.add_feedback({"advocate", std::nullopt, "manual", "+1", std::string("detail")});
    CHECK(!fb.session_id);
    CHECK_EQ(*fb.detail, std::string("detail"));
    CHECK_EQ(personas.feedback_for("advocate").size(), std::size_t(1));
    CHECK_EQ(json_stringify(to_json(fb)).find("\"session_id\":null") != std::string::npos, true);

    rules.bulk_seed(seeds::DISCUSSION_RULE_SEEDS());
    CHECK(!rules.list({true, std::string("tick")}).empty());
    CHECK(!rules.list({true, std::string("event")}).empty());
    auto propose = rules.get("propose-on-gap");
    CHECK_EQ(*propose->event_kind, std::string("DesignGapDetected"));
    CHECK_EQ(*propose->target, std::string("advocate"));
    CHECK_EQ(propose->added_by, std::string("seed"));
    CHECK_EQ(propose->conditions, std::string("[]"));

    rules.log({std::string("propose-on-gap"), "fire", "engine", std::string("test")});
    auto logs = rules.recent_logs(10);
    CHECK_EQ(logs.size(), std::size_t(1));
    CHECK_EQ(logs[0].action, std::string("fire"));
    rules.set_last_fired("propose-on-gap", time::now_ms() / 1000);
    CHECK(rules.get("propose-on-gap")->last_fired_at.has_value());

    rules.set_enabled("propose-on-gap", false);
    bool found = false;
    for (const auto& r : rules.list({false, std::nullopt})) found = found || r.id == "propose-on-gap";
    CHECK(found);
    rules.set_enabled("propose-on-gap", true);
    rules.remove("propose-on-gap", "test", "trial");
    auto removed = rules.get("propose-on-gap");
    CHECK_EQ(removed->enabled, std::int64_t(0));
    CHECK_EQ(*removed->removed_reason, std::string("trial"));

    RuleSeed stale;
    stale.id = "reconcile-target";
    stale.trigger_type = "tick";
    stale.target = "advocate";
    stale.instructions = "x";
    rules.insert_or_ignore(stale);
    CHECK(!rules.get("reconcile-target")->tick_sec);
    stale.tick_sec = 90;
    stale.cooldown_sec = 90;
    rules.reconcile_seed(stale);
    auto healed = rules.get("reconcile-target");
    CHECK_EQ(*healed->tick_sec, std::int64_t(90));
    CHECK_EQ(healed->cooldown_sec, std::int64_t(90));
    CHECK_EQ(healed->instructions, std::string("x"));
    CHECK_EQ(rules.list().size(), seeds::DISCUSSION_RULE_SEEDS().size() + 1);
}

TEST_CASE("schema DDL is identical to the TS migration (sqlite_master)") {
    auto db = sql::Database::open(":memory:");
    apply_persona_engine_migrations(*db);
    auto row = db->get("SELECT sql FROM sqlite_master WHERE name = 'session_rule_fires'");
    CHECK_EQ(row->str("sql"), std::string(R"(CREATE TABLE session_rule_fires (
        session_id TEXT NOT NULL,
        rule_id TEXT NOT NULL,
        count INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (session_id, rule_id)
      ))"));
    CHECK_EQ(db->table_columns("personas").size(), std::size_t(10));
    CHECK_EQ(db->table_columns("rules").size(), std::size_t(16));
}

TEST_CASE("learned_notes: broken JSON resets to [] like TS, oversized is refused (not wiped)") {
    auto db = sql::Database::open(":memory:");
    apply_persona_engine_migrations(*db);
    PersonasRepo personas(db);
    personas.insert_or_ignore({"p", "n", "d", "desc", {"t"}, "s"});
    db->run("UPDATE personas SET learned_notes = '{broken' WHERE id = 'p'");
    personas.append_learned_note("p", "a");
    CHECK_EQ(parse_json_or_throw(personas.get("p")->learned_notes).size(), std::size_t(1));
    const std::string big = "[\"" + std::string(learned_notes_limits().max_bytes, 'x') + "\"]";
    db->run("UPDATE personas SET learned_notes = ? WHERE id = 'p'", {big});
    CHECK_THROWS(personas.append_learned_note("p", "b"));
    CHECK_EQ(personas.get("p")->learned_notes.size(), big.size());
}

TEST_CASE("debate tick rules carry tick_sec and seed into the DB") {
    auto db = sql::Database::open(":memory:");
    apply_persona_engine_migrations(*db);
    RulesRepo rules(db);
    rules.bulk_seed(worker_pool::DEBATE_RULE_SEEDS());
    for (const auto& r : rules.list({std::nullopt, std::string("tick")})) CHECK(r.tick_sec && *r.tick_sec > 0);
}

namespace {
void run_store_contract(SessionFireStore& store) {
    CHECK_EQ(store.total("s1"), std::int64_t(0));
    CHECK_EQ(store.rule_count("s1", "r1"), std::int64_t(0));
    store.increment("s1", "r1");
    store.increment("s1", "r1");
    store.increment("s1", "r2");
    CHECK_EQ(store.rule_count("s1", "r1"), std::int64_t(2));
    CHECK_EQ(store.rule_count("s1", "r2"), std::int64_t(1));
    CHECK_EQ(store.total("s1"), std::int64_t(3));
    store.increment("s2", "r1");
    CHECK_EQ(store.total("s2"), std::int64_t(1));
    CHECK_EQ(store.total("s1"), std::int64_t(3));
    store.reset("s1");
    CHECK_EQ(store.total("s1"), std::int64_t(0));
    CHECK_EQ(store.total("s2"), std::int64_t(1));
}
} // namespace

TEST_CASE("session fire store contract (in-memory / sqlite)") {
    InMemorySessionFireStore mem;
    run_store_contract(mem);
    auto db = sql::Database::open(":memory:");
    apply_persona_engine_migrations(*db);
    SqliteSessionFireStore sqlite(db);
    run_store_contract(sqlite);
}

TEST_CASE("session fire counts survive restart (cap holds across reboot)") {
    const auto path = tmp_db();
    {
        auto db1 = sql::Database::open(path);
        apply_persona_engine_migrations(*db1);
        SqliteSessionFireStore s1(db1);
        s1.increment("live", "ruleX");
        s1.increment("live", "ruleX");
        s1.increment("live", "ruleY");
        CHECK_EQ(s1.total("live"), std::int64_t(3));
    }
    auto db2 = sql::Database::open(path);
    apply_persona_engine_migrations(*db2);
    SqliteSessionFireStore s2(db2);
    CHECK_EQ(s2.total("live"), std::int64_t(3));
    CHECK_EQ(s2.rule_count("live", "ruleX"), std::int64_t(2));
    CHECK_EQ(s2.rule_count("live", "ruleY"), std::int64_t(1));
    s2.increment("live", "ruleX");
    CHECK_EQ(s2.rule_count("live", "ruleX"), std::int64_t(3));
}

DI_TEST_MAIN()
