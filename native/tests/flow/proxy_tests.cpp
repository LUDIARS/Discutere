// src/flow/proxy/{contract,attribution,prompt}.ts の移植の確認 (tests/flow/proxy-debate.test.ts の契約・帰属の観点 + golden)。
#include "w1e_fixture.hpp"

#include "flow/proxy/attribution.hpp"
#include "flow/proxy/contract.hpp"
#include "flow/proxy/prompt.hpp"

using namespace di;
using namespace di::flow;
using namespace di::flow::proxy;

TEST_CASE("persona pack validation matches Node (errors and order)") {
    for (const auto& c : w1e::pure()["packs"]) CHECK_EQ(w1e::s(to_json(parse_persona_pack(c["raw"]))), w1e::s(c["result"]));
}

TEST_CASE("speaker key and genre patterns") {
    CHECK(is_speaker_key("ext:steam:0123456789abcdef"));
    CHECK(!is_speaker_key("ext:steam:0123456789abcdeF"));
    CHECK(!is_speaker_key("ext:steam:0123456789abcde"));
    CHECK(!is_speaker_key("ext:1x:0123456789abcdef"));
    CHECK(!is_speaker_key("ext::0123456789abcdef"));
    CHECK(!is_speaker_key("ext:steam:0123456789abcdef\n"));
    CHECK(is_genre_id("a"));
    CHECK(is_genre_id(std::string(64, 'a')));
    CHECK(!is_genre_id(std::string(65, 'a')));
    CHECK(!is_genre_id("_a"));
}

TEST_CASE("huge / deep packs do not crash") {
    Json deep = Json::object();
    Json* cur = &deep;
    for (int i = 0; i < 60; ++i) {
        (*cur)["n"] = Json::object();
        cur = &(*cur)["n"];
    }
    (*cur)["url"] = "x";
    const auto v = parse_persona_pack(deep);
    CHECK(!v.ok);
    Json big = Json::object();
    big["evidence"] = Json::array();
    for (int i = 0; i < 10'000; ++i) big["evidence"].push_back(Json{{"id", "E1"}});
    CHECK(!parse_persona_pack(big).ok);
}

TEST_CASE("attribution matches Node") {
    const auto& a = w1e::pure()["attribution"];
    AttributionScope scope;
    scope.evidenceIds = a["scope"]["evidenceIds"].get<std::vector<std::string>>();
    for (auto it = a["scope"]["evidenceTexts"].begin(); it != a["scope"]["evidenceTexts"].end(); ++it)
        scope.evidenceTexts[it.key()] = it.value().get<std::string>();
    for (const auto& c : a["cases"]) {
        CheckAttributionArgs args;
        args.claimKind = w1e::opt(c["args"], "claimKind");
        args.evidenceIds = c["args"]["evidenceIds"].get<std::vector<std::string>>();
        args.data = c["args"]["data"].get<std::string>();
        args.scope = scope;
        CHECK_EQ(w1e::s(to_json(check_attribution(args))), w1e::s(c["result"]));
    }
    for (const auto& c : w1e::pure()["evidenceIds"]) CHECK_EQ(w1e::s(Json(coerce_evidence_ids(c["v"]))), w1e::s(c["r"]));
    for (const auto& c : w1e::pure()["claimKinds"]) {
        const auto k = coerce_claim_kind(c["v"]);
        CHECK_EQ(k ? Json(*k) : Json(nullptr), c["r"]);
    }
    for (const auto& c : w1e::pure()["quotes"])
        CHECK_EQ(quote_appears_in_evidence(c["d"].get<std::string>(), c["c"].get<std::vector<std::string>>()), c["r"].get<bool>());
    for (const auto& c : w1e::pure()["suffix"])
        CHECK_EQ(attribution_suffix(c["k"].get<std::string>(), c["ids"].get<std::vector<std::string>>()), c["r"].get<std::string>());
}

TEST_CASE("proxy prompt lines are byte-identical to Node") {
    const auto& p = w1e::pure()["proxyPrompt"];
    const FlowPersona persona = flow_persona_from_json(p["persona"]);
    CHECK_EQ(w1e::s(Json(proxy_prompt_lines(persona))), w1e::s(p["lines"]));
    CHECK_EQ(proxy_ground_fields(persona), p["fields"].get<std::string>());
    FlowPersona plain = persona;
    plain.proxy.reset();
    CHECK_EQ(w1e::s(Json(proxy_prompt_lines(plain))), w1e::s(p["plainLines"]));
    CHECK_EQ(proxy_ground_fields(plain), p["plainFields"].get<std::string>());
}

DI_TEST_MAIN()
