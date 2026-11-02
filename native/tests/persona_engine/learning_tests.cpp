// learner.ts / metrics.ts の移植の検証: tests/persona-engine/learning.test.ts の観点 + Node golden。
#include "check.hpp"

#include <nlohmann/json.hpp>

#include "persona_engine/db/migrations.hpp"
#include "persona_engine/db/personas_repo.hpp"
#include "persona_engine/db/rules_repo.hpp"
#include "persona_engine/learning/learner.hpp"
#include "persona_engine/learning/metrics.hpp"
#include "persona_engine/seeds/personas.hpp"
#include "w2i_support.hpp"

using namespace di;
using namespace di::persona_engine;
using namespace di::persona_engine::learning;

namespace {
nlohmann::json plain(const Json& j) { return nlohmann::json::parse(j.dump()); }
} // namespace

TEST_CASE("PROPOSE_PATTERN: Node の正規表現と同じ一致") {
    for (const auto& c : w2i::load_fixture("propose_pattern.json")) {
        const auto m = match_propose_pattern(c["detail"].get<std::string>());
        if (c["match"].is_null()) {
            CHECK(!m);
        } else {
            CHECK(m.has_value());
            CHECK_EQ(m->personaId, c["match"]["personaId"].get<std::string>());
            CHECK_EQ(m->hypothesisId, c["match"]["hypothesisId"].get<std::string>());
        }
    }
    // 巨大な detail でも線形に近い時間で終わる
    std::string big;
    for (int i = 0; i < 20000; ++i) big += "persona x ";
    CHECK(!match_propose_pattern(big));
}

TEST_CASE("learning: Node golden (record / notes / personaMetrics / ruleMetrics)") {
    const auto fx = w2i::load_fixture("learning.json");
    auto db = w2i::temp_db("learning");
    db::apply_persona_engine_migrations(*db);
    db::PersonasRepo personas(db);
    personas.bulk_seed(seeds::DISCUSSION_PERSONA_SEEDS());
    db::RulesRepo rules(db);
    rules.log({std::string("propose-on-gap"), "fire", "ai", std::string("persona advocate proposed hypothesis h-0000000001")});
    rules.log({std::string("propose-on-gap"), "fire", "ai", std::string("persona advocate proposed hypothesis h-2")});
    rules.log({std::string("propose-on-gap"), "fire", "ai", std::string("persona sceptic proposed hypothesis h-3")});
    rules.log({std::string("refute-cold"), "skip", "engine", std::string("cooldown")});
    rules.log({std::string("refute-cold"), "error", "ai", std::string("invalid JSON")});
    rules.log({std::string("x-rule"), "fire", "ai", std::string("persona ghost proposed hypothesis h-4")});
    rules.log({std::nullopt, "fire", "ai", std::string("no rule id")});

    std::string stmt;
    for (int i = 0; i < 10; ++i) stmt += "選択肢を2つに絞る";
    const std::vector<RecordOutcomeArgs> calls = {
        {&personas, &rules, "h-0000000001", "integrated", stmt, std::nullopt},
        {&personas, &rules, "h-2", "rejected", std::nullopt, std::nullopt},
        {&personas, &rules, "h-2", "integrated", std::string(""), std::nullopt},
        {&personas, &rules, "h-4", "integrated", std::nullopt, std::nullopt},
        {&personas, &rules, "h-none", "integrated", std::nullopt, std::nullopt},
    };
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const auto r = record_hypothesis_outcome(calls[i]);
        const auto& e = fx["outcomes"][i];
        CHECK_EQ(r.recorded, e["recorded"].get<bool>());
        if (e.contains("personaId")) CHECK_EQ(*r.personaId, e["personaId"].get<std::string>());
        else CHECK(!r.personaId);
    }
    const auto notes = parse_json_or_throw(personas.get("advocate")->learned_notes);
    CHECK_EQ(notes.size(), fx["advocateNotes"].size());
    for (std::size_t i = 0; i < notes.size(); ++i) CHECK_EQ(notes[i]["note"].get<std::string>(), fx["advocateNotes"][i].get<std::string>());

    auto status = [](const std::string& h) -> std::string {
        if (h == "h-0000000001") return "integrated";
        if (h == "h-2") return "rejected";
        return "pending";
    };
    Json pm = Json::array();
    for (const auto& m : persona_metrics(personas, rules, status)) pm.push_back(to_json(m));
    CHECK(plain(pm) == plain(fx["personaMetrics"]));
    Json rm = Json::array();
    for (const auto& m : rule_metrics(rules)) rm.push_back(to_json(m));
    CHECK(plain(rm) == plain(fx["ruleMetrics"]));
}

TEST_CASE("learning: persona の書き込みに失敗したら recorded=false") {
    auto db = w2i::temp_db("learning2");
    db::apply_persona_engine_migrations(*db);
    db::PersonasRepo personas(db);
    db::RulesRepo rules(db);
    rules.log({std::string("r"), "fire", "ai", std::string("persona missing proposed hypothesis h-1")});
    const auto r = record_hypothesis_outcome({&personas, &rules, "h-1", "integrated", std::nullopt, std::nullopt});
    CHECK(!r.recorded);
    CHECK_EQ(format_note("integrated", "abcdefghijk", std::nullopt), std::string("✅ integrated: hyp:abcdefgh"));
}

DI_TEST_MAIN()
