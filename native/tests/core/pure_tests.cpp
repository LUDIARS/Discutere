// core の純関数・小さな状態 (command-parser / responds-to / state-machine / caches / noise exclusions)。
// tests/core/projection/command-parser.test.ts, tests/core/hypothesis/state-machine.test.ts + golden。
#include "core/test_util.hpp"

#include "core/cache/hotspot_rank.hpp"
#include "core/cache/uncategorized_clusters.hpp"
#include "core/hypothesis/state_machine.hpp"
#include "core/noise/exclusions.hpp"
#include "core/projection/command_parser.hpp"
#include "core/projection/responds_to_inference.hpp"

using namespace di;
using namespace di::core;

TEST_CASE("parseCommand (TS assertions)") {
    CHECK(!projection::parse_command("hello"));
    auto d = projection::parse_command("/define Jump Arc");
    CHECK(d && d->command == "define" && d->args == std::vector<std::string>({"Jump", "Arc"}));
    auto s = projection::parse_command(" /SUBMIT   ");
    CHECK(s && s->command == "submit" && s->args.empty());
}

TEST_CASE("golden: parseCommand / inferRespondsTo / canTransition") {
    const Json fx = test::load_fixture("core/pure.json");
    for (const auto& c : fx["commands"]) {
        auto r = projection::parse_command(c["in"].get<std::string>());
        if (c["out"].is_null()) {
            CHECK(!r);
            continue;
        }
        CHECK(r.has_value());
        CHECK_EQ(r->command, c["out"]["command"].get<std::string>());
        CHECK_EQ(Json(r->args), c["out"]["args"]);
    }
    for (const auto& c : fx["responds"]) {
        auto r = projection::infer_responds_to(c["in"].get<std::string>());
        CHECK_EQ(r ? Json(*r) : Json(), c["out"]);
    }
    for (const auto& c : fx["transitions"]) {
        auto r = hypothesis::can_transition(c["from"].get<std::string>(), c["action"].get<std::string>());
        CHECK_EQ(r.ok, c["out"]["ok"].get<bool>());
        if (r.ok) CHECK_EQ(r.to, c["out"]["to"].get<std::string>());
        else CHECK_EQ(r.error, c["out"]["error"].get<std::string>());
    }
}

TEST_CASE("caches keep the last value per workspace") {
    CHECK(!cache::get_hotspot_rank_cache("ws"));
    cache::set_hotspot_rank_cache("ws", {{"m1", "gacha", 3, 1, 0, 2.5}});
    CHECK_EQ(cache::get_hotspot_rank_cache("ws")->at(0).name, std::string("gacha"));
    CHECK_EQ(json_stringify(cache::to_json(cache::get_hotspot_rank_cache("ws")->at(0))),
             std::string(R"({"mechanicId":"m1","name":"gacha","utteranceCount":3,"gapCount":1,"staleCount":0,"score":2.5})"));
    CHECK(!cache::get_uncategorized_clusters_cache("ws"));
    cache::set_uncategorized_clusters_cache("ws", {{"k", 2, {"a", "b"}}});
    CHECK_EQ(json_stringify(cache::to_json(cache::get_uncategorized_clusters_cache("ws")->at(0))),
             std::string(R"({"key":"k","count":2,"sample":["a","b"]})"));
}

TEST_CASE("noise exclusions are reversible and idempotent") {
    test::TempDir dir;
    auto db = sql::Database::open(dir.file("x.db"));
    CHECK(!noise::is_excluded(*db, "u1"));
    noise::exclude_utterance(*db, "u1", std::string("off-topic"));
    noise::exclude_utterance(*db, "u1");
    noise::exclude_utterance(*db, "u2");
    CHECK(noise::is_excluded(*db, "u1"));
    CHECK_EQ(noise::list_excluded_ids(*db).size(), 2u);
    CHECK(db->get("SELECT reason FROM utterance_exclusions WHERE utterance_id='u1'")->is_null("reason"));
    noise::restore_utterance(*db, "u1");
    CHECK(!noise::is_excluded(*db, "u1"));
    db->exec("CREATE TABLE utterances (id TEXT)");
    db->exec("INSERT INTO utterances VALUES ('u2'), ('u3')");
    CHECK_EQ(db->all(std::string("SELECT id FROM utterances WHERE ") + noise::EXCLUDED_NOT_IN_SQL).size(), 1u);
}

DI_TEST_MAIN()
