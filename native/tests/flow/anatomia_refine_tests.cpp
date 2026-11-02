// w2-H: src/flow/anatomia/{refine,index}.ts の移植の確認 (tests/flow/anatomia-refine.test.ts + golden)。
#include "w2h_support.hpp"

#include "flow/anatomia/index.hpp"
#include "flow/anatomia/refine.hpp"
#include "persona_engine/llm/mock.hpp"

using namespace di;
using namespace di::flow;
using namespace di::flow::anatomia;

namespace {

std::vector<AnatomiaDomainCard> cards() {
    std::vector<AnatomiaDomainCard> out;
    for (const auto& c : w2h::fx()["anatomia"]["cards"]) {
        AnatomiaDomainCard d;
        d.name = json_string(c, "name");
        d.description = json_string(c, "description");
        for (const auto& m : json_array(c, "mechanics")) d.mechanics.push_back(m.get<std::string>());
        d.rationale = json_opt_string(c, "rationale");
        out.push_back(std::move(d));
    }
    return out;
}

Json mechanics_json(const std::vector<MechanicSummary>& ms) {
    Json a = Json::array();
    for (const auto& m : ms) {
        Json o = Json{{"name", m.name}, {"description", m.description}};
        if (m.intended_affect) o["intended_affect"] = *m.intended_affect;
        a.push_back(o);
    }
    return a;
}

persona_engine::MockResponder reply(const std::string& text) {
    return [text](const persona_engine::LLMInvokeArgs&) { return std::variant<std::string, persona_engine::LLMResult>(text); };
}

} // namespace

TEST_CASE("fallback / prompt / normalisation are the same as Node (golden)") {
    const auto& g = w2h::fx()["anatomia"];
    CHECK_EQ(w2h::s(mechanics_json(fallback_domains_to_mechanics(cards()))), w2h::s(g["fallback"]));
    CHECK_EQ(build_refine_prompt(cards(), std::string("Pagus の面白さ")), g["prompt"].get<std::string>());
    CHECK_EQ(build_refine_prompt(cards(), std::nullopt), g["promptNoTheme"].get<std::string>());
    for (const auto& c : g["refined"]) {
        persona_engine::MockLLMClient llm({reply(c["response"].get<std::string>())});
        std::vector<std::string> warns;
        const auto out = refine_domains_to_mechanics(cards(), {&llm, std::string("Pagus の面白さ"), std::nullopt,
                                                               [&](const std::string& m) { warns.push_back(m); }});
        CHECK_EQ(w2h::s(mechanics_json(out)), w2h::s(c["out"]));
        CHECK_EQ(w2h::s(Json(warns)), w2h::s(c["warns"]));
    }
}

TEST_CASE("no llm / empty cards / llm failure degrade (anatomia-refine.test.ts)") {
    CHECK_EQ(refine_domains_to_mechanics(cards()).size(), 2u);
    persona_engine::MockLLMClient unused({});
    CHECK(refine_domains_to_mechanics({}, {&unused, std::nullopt, std::nullopt, {}}).empty());
    CHECK_EQ(unused.calls(), 0);
    persona_engine::MockLLMClient fail({[](const persona_engine::LLMInvokeArgs&) {
        return std::variant<std::string, persona_engine::LLMResult>(persona_engine::LLMResult::failure("boom"));
    }});
    std::vector<std::string> warns;
    const auto out = refine_domains_to_mechanics(cards(), {&fail, std::nullopt, std::string("m"), [&](const std::string& m) { warns.push_back(m); }});
    CHECK_EQ(out[0].name, std::string("Trial & Judgment"));
    CHECK_EQ(*fail.last_invocation()->model, std::string("m"));
    CHECK(warns[0].find("boom") != std::string::npos);
}

TEST_CASE("boundary: oversized / deep / huge LLM responses fall back") {
    std::string big = "[{\"name\":\"" + std::string(kRefineResponseMaxBytes, 'a') + "\"}]";
    CHECK(!parse_refine_response(big));
    CHECK(!parse_refine_response(std::string(10'000, '[') + std::string(10'000, ']')));
    const std::string exact = "[" + std::string(kRefineResponseMaxBytes - 2, ' ') + "]";
    CHECK(parse_refine_response(exact).has_value());
    CHECK(!parse_refine_response("] x ["));
    CHECK(!parse_refine_response(""));
}

TEST_CASE("index: resolve source and fetch + refine through the runner") {
    CHECK(!resolve_anatomia_source(std::string("  "), std::nullopt));
    CHECK(resolve_anatomia_source(std::string(" p "), std::string("/r"))->kind == AnatomiaSource::Kind::project);
    CHECK_EQ(resolve_anatomia_source(std::string(" p "), std::string("/r"))->value, std::string("p"));
    CHECK_EQ(resolve_anatomia_source(std::nullopt, std::string(" /r "))->value, std::string("/r"));
    CHECK(!resolve_anatomia_source(std::nullopt, std::nullopt));

    w2h::TempFlowDb tmp;
    const std::string bin = tmp.file("anatomia.mjs");
    fs::write_file_atomic(bin, "// fake");
    GetAnatomiaMechanicsDeps deps;
    deps.binPath = bin;
    deps.runner = [](const std::string&, const std::vector<std::string>&, std::chrono::milliseconds, const std::string&) {
        return CliResult{json_stringify(Json{{"dir", "/x"},
                                             {"domains", Json::array({Json{{"name", "A"}, {"description", "d"}, {"mechanics", {"m1"}}, {"specRefs", Json::array()}}})}}),
                         0};
    };
    const auto ms = get_anatomia_mechanics(AnatomiaSource::project("p"), deps);
    CHECK_EQ(ms.size(), 1u);
    CHECK_EQ(ms[0].description, std::string("d (実装メカニクス: m1)"));
}

DI_TEST_MAIN()
