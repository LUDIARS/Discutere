// discussion-redo / sparring-commands / dialectic(convergence, premise-rules) / persona-import-document の移植の確認
// (tests/flow/discussion-redo.test.ts, sparring.test.ts, conclusion-convergence.test.ts の観点 + golden)。
#include "w1e_fixture.hpp"

#include "flow/dialectic/convergence.hpp"
#include "flow/dialectic/premise_rules.hpp"
#include "flow/discussion_redo.hpp"
#include "flow/persona_import_document.hpp"
#include "flow/sparring_commands.hpp"

using namespace di;
using namespace di::flow;

TEST_CASE("redo text and decisions match Node") {
    const auto& r = w1e::pure()["redo"];
    for (const auto& c : r["texts"]) CHECK_EQ(is_redo_text(c["t"].get<std::string>()), c["r"].get<bool>());
    for (const auto& c : r["decisions"]) {
        RedoInput in;
        if (!c["input"]["paper"].is_null())
            in.paper = RedoPaper{c["input"]["paper"]["status"].get<std::string>(), c["input"]["paper"]["flow"].get<std::string>()};
        in.concluded = c["input"]["concluded"].get<bool>();
        in.running = c["input"]["running"].get<bool>();
        const auto d = decide_redo(in);
        CHECK_EQ(d.ok, c["result"]["ok"].get<bool>());
        if (!d.ok) CHECK_EQ(d.reason, c["result"]["reason"].get<std::string>());
    }
}

TEST_CASE("sparring commands match Node") {
    for (const auto& c : w1e::pure()["sparring"]) {
        const std::string t = c["text"].get<std::string>();
        if (detect_sparring_command(t) != c["result"].get<std::string>()) throw test::Failure("sparring: " + t);
    }
    CHECK_EQ(detect_sparring_command(std::string(1u << 20, 'x') + "おわり"), std::string("end"));
}

TEST_CASE("convergence matches Node") {
    for (const auto& c : w1e::pure()["convergence"]) {
        const auto& in = c["input"];
        dialectic::ConvergenceInput ci;
        ci.unresolvedRebuttals = in["unresolvedRebuttals"].get<double>();
        if (!in["winnerShare"].is_null()) ci.winnerShare = in["winnerShare"].get<double>();
        ci.convergeShare = in["convergeShare"].get<double>();
        ci.newClaimsInWindow = in["newClaimsInWindow"].get<double>();
        ci.ratifiedAufhebenCount = in["ratifiedAufhebenCount"].get<double>();
        ci.aufhebungTarget = in["aufhebungTarget"].get<double>();
        CHECK_EQ(w1e::s(dialectic::to_json(dialectic::assess_convergence(ci))), w1e::s(c["result"]));
    }
}

TEST_CASE("premise rules text and coercion match Node") {
    const auto& p = w1e::pure()["premise"];
    CHECK_EQ(dialectic::premise_rules_text(), p["text"].get<std::string>());
    for (std::size_t i = 0; i < p["inputs"].size(); ++i) {
        const auto k = dialectic::coerce_inference_kind(p["inputs"][i]);
        CHECK_EQ(k ? Json(*k) : Json(nullptr), p["inference"][i]);
        const auto a = dialectic::coerce_attack_point(p["inputs"][i]);
        CHECK_EQ(a ? Json(*a) : Json(nullptr), p["attack"][i]);
    }
    CHECK_EQ(dialectic::inference_kind_label("abduction"), std::string("アブダクション"));
}

TEST_CASE("persona documents match Node") {
    for (const auto& c : w1e::pure()["docs"]) {
        const auto doc = parse_persona_document(c["d"].get<std::string>());
        CHECK_EQ(w1e::s(Json(doc.personas)), w1e::s(c["r"]["personas"]));
        CHECK_EQ(doc.invalidJsonLines, c["r"]["invalidJsonLines"].get<int>());
    }
}

TEST_CASE("persona document bounds") {
    // 深すぎる 1 文書は全体として読まず、行ごとにも読めない → 不正行 1
    const std::string deep = std::string(100, '[') + std::string(100, ']');
    CHECK_EQ(parse_persona_document(deep).invalidJsonLines, 1);
    // 上限 (16MiB) を超える 1 行は読まない
    const std::string big = "{\"a\":\"" + std::string((16u << 20), 'x') + "\"}";
    const auto d = parse_persona_document(big);
    CHECK(d.personas.empty());
    CHECK_EQ(d.invalidJsonLines, 1);
    // 多数の行
    std::string many;
    for (int i = 0; i < 50'000; ++i) many += "{\"i\":" + std::to_string(i) + "}\nbad\n";
    const auto m = parse_persona_document(many);
    CHECK_EQ(m.personas.size(), std::size_t{50'000});
    CHECK_EQ(m.invalidJsonLines, 50'000);
}

DI_TEST_MAIN()
