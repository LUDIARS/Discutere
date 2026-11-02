// src/flow/personas.ts / turn-scheduler.ts / persona-display.ts / tags.ts の移植の確認
// (tests/flow/turn-scheduler.test.ts, model-roster.test.ts, persona-display.test.ts の観点 + golden)。
#include "w1e_fixture.hpp"

#include <set>

#include "flow/persona_display.hpp"
#include "flow/personas.hpp"
#include "flow/tags.hpp"
#include "flow/turn_scheduler.hpp"

using namespace di;
using namespace di::flow;

TEST_CASE("generate personas matches Node (ids aside)") {
    for (const auto& run : w1e::pure()["personas"]) {
        const auto& a = run["args"];
        GeneratePersonasArgs args;
        args.count = a["count"].get<int>();
        args.defaultModel = "def";
        args.isLocal = args.count % 2 == 0;
        args.rng = w1e::lcg(a["rngSeed"].get<std::uint32_t>());
        if (a.contains("roster")) {
            FlowModelRoster r;
            r.facilitator = a["roster"]["facilitator"].get<std::string>();
            r.discussants = a["roster"]["discussants"].get<std::vector<std::string>>();
            args.roster = r;
        }
        const auto ps = generate_flow_personas(args);
        Json arr = Json::array(), stances = Json::array();
        std::set<std::string> ids;
        for (const auto& p : ps) {
            Json j = to_json(p);
            CHECK_EQ(j["id"].get<std::string>().size(), std::size_t{36});
            ids.insert(p.id);
            j.erase("id");
            arr.push_back(j);
            stances.push_back(persona_stance(p));
            CHECK_EQ(w1e::s(to_json(flow_persona_from_json(to_json(p)))), w1e::s(to_json(p)));
        }
        CHECK_EQ(ids.size(), ps.size());
        CHECK_EQ(w1e::s(arr), w1e::s(run["personas"]));
        CHECK_EQ(w1e::s(stances), w1e::s(run["stances"]));
    }
    CHECK_THROWS(generate_flow_personas({0, "m", false, w1e::lcg(1), std::nullopt}));
    const auto& rc = w1e::pure()["rosterCounts"];
    CHECK_EQ(roster_persona_count(4, FlowModelRoster{std::nullopt, {"a", "b", "c"}}), rc[0].get<int>());
    CHECK_EQ(roster_persona_count(9, std::nullopt), rc[1].get<int>());
    CHECK_EQ(roster_persona_count(9, FlowModelRoster{}), rc[2].get<int>());
}

TEST_CASE("turn order matches Node and has no consecutive speaker") {
    auto ps = generate_flow_personas({6, "m", false, w1e::lcg(9), std::nullopt});
    for (std::size_t i = 0; i < ps.size(); ++i) ps[i].id = "p" + std::to_string(i);
    for (const auto& c : w1e::pure()["order"]) {
        const int turns = c["turns"].get<int>();
        Json ids = Json::array();
        for (const auto& p : build_turn_order(ps, turns, w1e::lcg(static_cast<std::uint32_t>(turns + 100)))) ids.push_back(p.id);
        CHECK_EQ(w1e::s(ids), w1e::s(c["ids"]));
    }
    CHECK_EQ(w1e::s(Json(shuffle(std::vector<int>{1, 2, 3, 4, 5, 6, 7}, w1e::lcg(77)))), w1e::s(w1e::pure()["shuffled"]));
    auto four = generate_flow_personas({4, "m", false, w1e::lcg(3), std::nullopt});
    for (std::uint32_t seed = 1; seed <= 50; ++seed) {
        const auto order = build_turn_order(four, 9, w1e::lcg(seed));
        CHECK_EQ(order.size(), std::size_t{9});
        for (std::size_t i = 1; i < order.size(); ++i) CHECK(order[i].id != order[i - 1].id);
        for (const auto& p : order) CHECK(p.role != "facilitator");
    }
    // rng が範囲外の値を返しても範囲外を引かない
    CHECK_EQ(build_turn_order(four, 3, [] { return 1.0; }).size(), std::size_t{3});
}

TEST_CASE("display names match Node") {
    for (const auto& c : w1e::pure()["display"]) {
        const auto& in = c["input"];
        DisplayNameInput d{in["name"].get<std::string>(), w1e::opt(in, "stance"), w1e::opt(in, "role"), w1e::opt(in, "possessionName")};
        CHECK_EQ(compose_display_name(d), c["result"].get<std::string>());
    }
    CHECK_EQ(stance_label("pro"), std::string("賛成派"));
    CHECK_EQ(role_label("debater"), std::string("論者"));
}

TEST_CASE("tags match Node") {
    for (const auto& c : w1e::pure()["tags"]) {
        const auto tags = c["tags"].get<std::vector<std::string>>();
        CHECK_EQ(can_collect_external(tags), c["can"].get<bool>());
        CHECK_EQ(paper_supplement(tags), c["supplement"].get<std::string>());
        CHECK_EQ(wants_foresight(tags), c["foresight"].get<bool>());
        CHECK_EQ(wants_proxy(tags), c["proxy"].get<bool>());
        // 機密/内部では蓄積済みの外部の声も渡さない
        CHECK_EQ(gate_external_voices(tags, std::optional<int>(1)).has_value(), c["can"].get<bool>());
    }
}

DI_TEST_MAIN()
