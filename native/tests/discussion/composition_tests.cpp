// tests/discussion/composition.test.ts の移植 + golden (native/tests/golden/w1-B-discussion.gen.ts)。
#include "check.hpp"

#include <set>

#include "base/fsutil.hpp"
#include "discussion/composition.hpp"

using namespace di;
using namespace di::discussion;

namespace {
const PartyConfig CFG{20, 2, "claude-opus-5", "claude-opus-5", {{"claude-sonnet-5", 6}, {"claude-haiku-4-5-20251001", 4}}, 4};
Rng seq_rng(std::vector<double> values) {
    auto i = std::make_shared<std::size_t>(0);
    return [values, i] { return values[(*i)++ % values.size()]; };
}
Json load() {
    auto text = fs::read_file(test::fixtures_dir() + "/discussion/cases.json");
    if (!text) throw std::runtime_error("fixture missing");
    return parse_json_or_throw(*text, {8u << 20, 1'000'000, 64});
}
PartyConfig cfg_from(const Json& j) {
    PartyConfig c;
    c.expectedUtterances = j["expectedUtterances"].get<double>();
    c.keymanCount = j["keymanCount"].get<int>();
    c.facilitatorModel = j["facilitatorModel"].get<std::string>();
    c.keymanModel = j["keymanModel"].get<std::string>();
    for (const auto& m : j["opinionModels"]) c.opinionModels.push_back({m["model"].get<std::string>(), m["weight"].get<double>()});
    c.minTotal = j["minTotal"].get<int>();
    return c;
}
Json party_json(const std::vector<PartyMember>& p) {
    Json a = Json::array();
    for (const auto& m : p) a.push_back(to_json(m));
    return a;
}
} // namespace

TEST_CASE("partySize") {
    CHECK_EQ(party_size(20), 6);
    CHECK_EQ(party_size(12), 4);
    CHECK_EQ(party_size(8), 4);
    CHECK_EQ(party_size(100), 26);
}

TEST_CASE("pickWeightedModel") {
    CHECK_EQ(pick_weighted_model(CFG.opinionModels, [] { return 0.0; }), std::string("claude-sonnet-5"));
    CHECK_EQ(pick_weighted_model(CFG.opinionModels, [] { return 0.99; }), std::string("claude-haiku-4-5-20251001"));
    CHECK_EQ(pick_weighted_model(CFG.opinionModels, [] { return 0.5; }), std::string("claude-sonnet-5"));
    CHECK_EQ(pick_weighted_model({}, [] { return 0.5; }), std::string(""));
}

TEST_CASE("formParty (6 members, model assignment, unique names)") {
    auto party = form_party(CFG, seq_rng({0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.05}));
    CHECK_EQ(party.size(), std::size_t(6));
    int fac = 0, key = 0, op = 0;
    std::set<std::string> names;
    for (const auto& m : party) {
        fac += m.tier == "facilitator";
        key += m.tier == "keyman";
        op += m.tier == "opinion";
        if (m.tier != "opinion") CHECK_EQ(m.model, std::string("claude-opus-5"));
        names.insert(m.personaName);
    }
    CHECK_EQ(fac, 1);
    CHECK_EQ(key, 2);
    CHECK_EQ(op, 3);
    CHECK_EQ(names.size(), party.size());
}

TEST_CASE("swapHalf (facilitator fixed / 2 swapped / keyman keeps Opus)") {
    auto party = form_party(CFG, seq_rng({0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.05}));
    auto swapped = swap_half(party, CFG, seq_rng({0.9, 0.1, 0.5, 0.3, 0.7, 0.2, 0.4}));
    CHECK(swapped[0] == party[0]);
    int changed = 0;
    for (std::size_t i = 0; i < party.size(); ++i) changed += swapped[i].personaName != party[i].personaName;
    CHECK_EQ(changed, 2);
    for (const auto& m : swapped)
        if (m.tier == "keyman") CHECK_EQ(m.model, std::string("claude-opus-5"));
}

TEST_CASE("decideStance") {
    auto party = form_party(CFG, seq_rng({0.1}));
    CHECK_EQ(decide_stance(party[0], [] { return 0.1; }), std::string("neutral"));
    CHECK_EQ(decide_stance(party[1], [] { return 0.1; }), std::string("pro"));
    CHECK_EQ(decide_stance(party[1], [] { return 0.9; }), std::string("con"));
}

TEST_CASE("formParty / swapHalf / sizes / picks match Node golden (incl. name pool exhaustion)") {
    const Json all = load();
    const std::vector<double> seq = {0.1, 0.5, 0.9, 0.2, 0.7, 0.3, 0.6, 0.4, 0.8, 0.05, 0.15, 0.25, 0.99, 0.0, 0.45};
    for (const auto& p : all["parties"]) {
        const auto cfg = cfg_from(p["cfg"]);
        auto rng = seq_rng(seq);
        auto party = form_party(cfg, rng);
        auto swapped = swap_half(party, cfg, rng);
        CHECK_EQ(json_stringify(party_json(party)), json_stringify(p["party"]));
        CHECK_EQ(json_stringify(party_json(swapped)), json_stringify(p["swapped"]));
    }
    for (const auto& s : all["sizes"]) CHECK_EQ(party_size(s["e"].get<double>()), s["size"].get<int>());
    const auto cfg0 = cfg_from(all["configs"][0]);
    for (const auto& s : all["picks"]) {
        const double r = s["r"].get<double>();
        CHECK_EQ(pick_weighted_model(cfg0.opinionModels, [r] { return r; }), s["model"].get<std::string>());
    }
}

DI_TEST_MAIN()
