// design-diagnostic (contracts / prompt-contract / validation) と level-check・mechanics-check の契約のテスト。
#include "base/strings.hpp"
#include "check.hpp"
#include "design_diagnostic/contracts.hpp"
#include "design_diagnostic/prompt_contract.hpp"
#include "design_diagnostic/validation.hpp"
#include "level_check/contracts.hpp"
#include "mechanics_check/contracts.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::design_diagnostic;

namespace {
std::string error_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const DiagnosticError& e) {
        return e.code() + "|" + e.what();
    }
    return "";
}
} // namespace

TEST_CASE("prompt contract matches Node") {
    const Json f = w1d::load_fixture("flow/premise.json");
    CHECK_EQ(diagnostic_prompt_contract(), f["promptContract"].get<std::string>());
}

TEST_CASE("validation primitives (strict, no coercion)") {
    const std::string code = "invalid_input";
    CHECK_EQ(error_of([&] { object(Json::array(), "$", {"a"}, code); }), std::string("invalid_input|$: expected object"));
    CHECK_EQ(error_of([&] { object(Json{{"b", 1}}, "$", {"a"}, code); }), std::string("invalid_input|$: unexpected field"));
    CHECK(error_of([&] { object(Json{{"a", 1}}, "$", {"a"}, code); }).empty());
    // 文字列: 上限ちょうど / +1 (UTF-16 単位) / 空白だけ
    CHECK(error_of([&] { string(Json(std::string(10, 'a')), "$.s", 10, code); }).empty());
    CHECK_EQ(error_of([&] { string(Json(std::string(11, 'a')), "$.s", 10, code); }),
             std::string("invalid_input|$.s: expected nonempty string <= 10 characters"));
    CHECK(error_of([&] { string(Json("あいう"), "$.s", 3, code); }).empty());
    CHECK(!error_of([&] { string(Json("😀😀"), "$.s", 3, code); }).empty());  // サロゲートペアは 2 単位
    CHECK(!error_of([&] { string(Json("  \n"), "$.s", 10, code); }).empty());
    CHECK(!error_of([&] { string(Json(3), "$.s", 10, code); }).empty());
    CHECK(error_of([&] { array(Json::array({1, 2}), "$.a", 2, code, 1); }).empty());
    CHECK_EQ(error_of([&] { array(Json::array({1, 2, 3}), "$.a", 2, code, 1); }), std::string("invalid_input|$.a: expected 1..2 items"));
    CHECK(!error_of([&] { array(Json::array(), "$.a", 2, code, 1); }).empty());
    CHECK_EQ(finite(Json(1.5), "$", code), 1.5);
    CHECK(!error_of([&] { finite(Json("1"), "$", code); }).empty());
    CHECK_EQ(error_of([&] { choice(Json("x"), {"a", "b"}, "$.c", code); }), std::string("invalid_input|$.c: expected a / b"));
    CHECK_EQ(id(Json("A1_.:-x"), "$.id", code), std::string("A1_.:-x"));
    CHECK_EQ(error_of([&] { id(Json("_bad"), "$.id", code); }), std::string("invalid_input|$.id: invalid ID"));
    CHECK(!error_of([&] { id(Json("a b"), "$.id", code); }).empty());
    CHECK(error_of([&] { id(Json(std::string(80, 'a')), "$.id", code); }).empty());
    CHECK(!error_of([&] { id(Json(std::string(81, 'a')), "$.id", code); }).empty());
    CHECK_EQ(error_of([&] { unique({"a", "b", "a"}, "$.ids", code); }), std::string("invalid_input|$.ids: duplicate ID"));
    CHECK(error_of([&] { unique({"a", "b"}, "$.ids", code); }).empty());
}

TEST_CASE("contracts serialize with TS keys") {
    KindedFinding f;
    f.kind = "contradiction";
    f.target = "t";
    f.left = {"l", {{"r1", "q"}}};
    f.right = {"r", {}};
    f.status = "inconsistent";
    f.causeDomain = "unknown";
    f.conditions = {"c"};
    DiagnosticResult r{{}, {f}};
    CHECK_EQ(json_stringify(to_json(r)),
             std::string(R"({"logic_differences":[],"design_gap":[{"kind":"contradiction","target":"t","left":{"text":"l","evidence":[{"referenceId":"r1","quote":"q"}]},"right":{"text":"r","evidence":[]},"status":"inconsistent","causeDomain":"unknown","conditions":["c"],"unknowns":[]}]})"));
    CHECK_EQ(DIAGNOSTIC_LIMITS.resultBytes, std::size_t(524'288));
    CHECK_EQ(level_check::LEVEL_CHECK_LIMITS.totalOutcomes, std::size_t(256));
    CHECK_EQ(level_check::LEVEL_CHECK_LIMITS.findingsPerKind, std::size_t(32));
    CHECK_EQ(mechanics_check::mechanics_difference_kinds().size(), std::size_t(4));
    CHECK_EQ(level_check::level_difference_kinds().size(), std::size_t(5));

    level_check::PlayEnvelopeDifference env;
    env.target = "x";
    env.status = "unknown";
    env.causeDomain = "unknown";
    env.envelope.scenarioId = "s1";
    env.envelope.metric = {"m", "M", "pt", "higher"};
    env.envelope.determinant = "random";
    env.envelope.rangeScope = "enumerated_partial";
    env.envelope.min = 1;
    env.envelope.max = 2.5;
    env.envelope.probabilityStatus = "non_exhaustive";
    const Json j = level_check::to_json(env);
    CHECK_EQ(j["kind"].get<std::string>(), std::string("play_envelope"));
    CHECK(j["envelope"]["skillBand"].is_null());
    CHECK(j["envelope"]["expectedValue"].is_null());
    CHECK_EQ(j["envelope"]["max"].get<double>(), 2.5);
}

DI_TEST_MAIN()
