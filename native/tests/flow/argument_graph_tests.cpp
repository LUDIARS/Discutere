// src/flow/argument-graph.ts の移植の確認 (tests/flow/argument-graph.test.ts の観点 + golden)。
#include "w1e_fixture.hpp"

#include "flow/argument_graph.hpp"

using namespace di;
using namespace di::flow;

namespace {
GraphUtterance u(const char* id, const char* persona, const char* act, std::optional<std::string> target, double turn) {
    return {id, persona, act, std::move(target), turn};
}
Json edge_json(const RebuttalEdge& e) {
    Json o = Json::object();
    o["rebutId"] = e.rebutId;
    o["targetId"] = e.targetId;
    o["byPersonaId"] = e.byPersonaId;
    o["targetPersonaId"] = e.targetPersonaId ? Json(*e.targetPersonaId) : Json(nullptr);
    o["state"] = e.state;
    return o;
}
} // namespace

TEST_CASE("coerceAct: null / unknown -> claim") {
    CHECK_EQ(coerce_act(std::nullopt), std::string("claim"));
    CHECK_EQ(coerce_act(std::string("garbage")), std::string("claim"));
    CHECK_EQ(coerce_act(std::string("rebut")), std::string("rebut"));
    CHECK_EQ(coerce_act(std::string("synthesize")), std::string("synthesize"));
}

TEST_CASE("transitions: rebut / defend / self-support / concede") {
    {
        ArgumentGraph g;
        g.apply(u("c1", "pro", "claim", std::nullopt, 1));
        g.apply(u("r1", "con", "rebut", "c1", 2));
        auto un = g.unresolved_rebuttals();
        CHECK_EQ(un.size(), std::size_t{1});
        CHECK_EQ(*un[0].targetPersonaId, std::string("pro"));
        g.apply(u("s1", "pro", "support", "r1", 3));
        CHECK(g.unresolved_rebuttals().empty());
        CHECK_EQ(g.rebuttal_by_id("r1")->state, std::string("defended"));
    }
    {
        ArgumentGraph g;
        g.apply(u("c1", "pro", "claim", std::nullopt, 1));
        g.apply(u("r1", "con", "rebut", "c1", 2));
        g.apply(u("s1", "con", "support", "r1", 3));
        CHECK_EQ(g.unresolved_rebuttals().size(), std::size_t{1});
    }
    {
        ArgumentGraph g;
        g.apply(u("c1", "pro", "claim", std::nullopt, 1));
        g.apply(u("r1", "con", "rebut", "c1", 2));
        g.apply(u("r2", "con", "rebut", "c1", 3));
        g.apply(u("x1", "pro", "concede", "c1", 4));
        CHECK(g.unresolved_rebuttals().empty());
        CHECK_EQ(g.concessions().size(), std::size_t{2});
    }
}

TEST_CASE("buildGraphFromRows: legacy act=null rows") {
    auto g = build_graph_from_rows({{"u1", "p1", std::nullopt, std::nullopt, 1},
                                    {"u2", "p2", std::nullopt, std::nullopt, 2},
                                    {"u3", "p2", std::string("rebut"), std::string("u1"), 3}});
    CHECK_EQ(g.new_claims_since(0), 2);
    CHECK_EQ(g.unresolved_rebuttals().size(), std::size_t{1});
    CHECK_EQ(g.unanswered_claims(), std::vector<std::string>{"u2"});
}

TEST_CASE("golden sequence matches Node") {
    const Json& gj = w1e::pure()["graph"];
    ArgumentGraph g;
    for (const auto& op : gj["ops"])
        g.apply({op[0].get<std::string>(), op[1].get<std::string>(), op[2].get<std::string>(),
                 op[3].is_null() ? std::nullopt : std::optional<std::string>(op[3].get<std::string>()), op[4].get<double>()});
    Json un = Json::array();
    for (const auto& e : g.unresolved_rebuttals()) un.push_back(edge_json(e));
    CHECK_EQ(w1e::s(un), w1e::s(gj["unresolved"]));
    CHECK_EQ(w1e::s(Json(g.unanswered_claims())), w1e::s(gj["unanswered"]));
    Json con = Json::array();
    for (const auto& c : g.concessions()) con.push_back(Json{{"concedeId", c.concedeId}, {"rebutId", c.rebutId}, {"targetId", c.targetId}});
    CHECK_EQ(w1e::s(con), w1e::s(gj["concessions"]));
    Json since = Json::array();
    for (double t : {0.0, 5.0, 10.0, 15.0}) since.push_back(g.new_claims_since(t));
    CHECK_EQ(w1e::s(since), w1e::s(gj["since"]));
    Json nodes = Json::array();
    for (const auto& n : g.claim_nodes()) nodes.push_back(Json{{"id", n.id}, {"personaId", n.personaId}, {"turn", n.turn}});
    CHECK_EQ(w1e::s(nodes), w1e::s(gj["nodes"]));
    Json counts = Json::array();
    for (const char* c : {"c1", "c2", "c3", "r1"}) counts.push_back(g.edge_count_for(c));
    CHECK_EQ(w1e::s(counts), w1e::s(gj["edgeCounts"]));
    Json targeting = Json::array();
    for (const auto& e : g.rebuttals_targeting("c1")) targeting.push_back(edge_json(e));
    CHECK_EQ(w1e::s(targeting), w1e::s(gj["targeting"]));
    CHECK_EQ(w1e::s(edge_json(*g.rebuttal_by_id("rr"))), w1e::s(gj["byId"][0]));
    CHECK(!g.rebuttal_by_id("zz").has_value());
}

DI_TEST_MAIN()
