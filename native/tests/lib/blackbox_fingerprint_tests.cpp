// @ludiars/blackbox fingerprint.test.ts の移植 + condition / canonicalJson の golden
// (native/tests/golden/w1-B-blackbox.gen.ts が記録した Node の出力)。
#include "check.hpp"

#include "base/fsutil.hpp"
#include "lib/blackbox/blackbox.hpp"

using namespace di;
using namespace di::lib::blackbox;

namespace {
Json load() {
    auto text = fs::read_file(test::fixtures_dir() + "/blackbox/cases.json");
    if (!text) throw std::runtime_error("fixture missing");
    return parse_json_or_throw(*text, {8u << 20, 1'000'000, 64});
}
Json cmp_dow(int v) { return Json{{"op", "cmp"}, {"feature", "dow"}, {"cmp", "=="}, {"value", v}}; }
} // namespace

TEST_CASE("fingerprint is independent of key order") {
    const Json out = Json{{"names", Json::array({"x"})}};
    const Json b = Json{{"value", 2}, {"cmp", "=="}, {"op", "cmp"}, {"feature", "dow"}};
    CHECK_EQ(rule_fingerprint(cmp_dow(2), out), rule_fingerprint(b, out));
}

TEST_CASE("different logical content gives different fingerprints") {
    const Json out = Json{{"names", Json::array({"x"})}};
    CHECK(rule_fingerprint(cmp_dow(2), out) != rule_fingerprint(cmp_dow(3), out));
}

TEST_CASE("sameOutput is deep equal independent of key order") {
    CHECK(same_output(Json{{"a", 1}, {"b", Json::array({1, 2})}}, Json{{"b", Json::array({1, 2})}, {"a", 1}}));
    CHECK(!same_output(Json{{"a", 1}}, Json{{"a", 2}}));
    CHECK(same_output(Json(), Json(nullptr)));
}

TEST_CASE("canonicalJson keeps array order") { CHECK(canonical_json(Json::array({2, 1})) != canonical_json(Json::array({1, 2}))); }

TEST_CASE("canonicalJson matches Node golden (index keys, UTF-16 order, numbers)") {
    for (const auto& c : Json(load()["canonical"])) CHECK_EQ(canonical_json(c["value"]), c["json"].get<std::string>());
}

TEST_CASE("validateCondition / describeCondition match Node golden (errors and depth limit)") {
    int n = 0;
    for (const auto& c : Json(load()["validation"])) {
        const std::string label = "case " + std::to_string(n++);
        if (c["ok"].get<bool>()) {
            Json cond;
            try {
                cond = validate_condition(c["input"]);
            } catch (const std::exception& e) {
                throw test::Failure(label + " threw " + e.what());
            }
            CHECK_EQ(json_stringify(cond), json_stringify(c["cond"]));
            CHECK_EQ(describe_condition(cond), c["text"].get<std::string>());
        } else {
            std::string error;
            try {
                validate_condition(c["input"]);
            } catch (const std::invalid_argument& e) {
                error = e.what();
            }
            if (error != c["error"].get<std::string>())
                throw test::Failure(label + ": got '" + error + "' expected '" + c["error"].get<std::string>() + "'");
        }
    }
}

TEST_CASE("evaluate matches Node golden") {
    const Json all = load();
    for (const auto& e : all["evals"]) {
        std::size_t i = 0;
        for (const auto& f : all["features"]) CHECK_EQ(evaluate(e["cond"], f), e["results"][i++].get<bool>());
    }
}

TEST_CASE("ruleFingerprint matches Node golden") {
    for (const auto& c : Json(load()["fingerprints"])) CHECK_EQ(rule_fingerprint(c["when"], c["output"]), c["fingerprint"].get<std::string>());
}

TEST_CASE("evaluate is robust to malformed / deep conditions from storage") {
    CHECK(!evaluate(Json{{"op", "and"}}, Json{{"x", 1}}));
    CHECK(!evaluate(Json::array(), Json{{"x", 1}}));
    CHECK(!evaluate(Json{{"op", "cmp"}, {"feature", "x"}, {"cmp", ">"}}, Json{{"x", 1}}));
    // 深い入れ子 (評価上限 64 段を超える) は false で止まる (再帰で落ちない)。
    Json deep = Json{{"op", "cmp"}, {"feature", "x"}, {"cmp", "=="}, {"value", 1}};
    for (int i = 0; i < 200; ++i) deep = Json{{"op", "not"}, {"clause", deep}};
    CHECK(!evaluate(deep, Json{{"x", 1}}));
    CHECK_EQ(describe_condition(Json{{"op", "zzz"}}), std::string("?"));
    // validate は 8 段まで (9 段目で拒否)。
    CHECK_THROWS(validate_condition(deep));
}

TEST_CASE("canonicalJson rejects absurdly deep values instead of overflowing the stack") {
    Json deep = Json::array();
    for (int i = 0; i < 1000; ++i) deep = Json::array({deep});
    CHECK_THROWS(canonical_json(deep));
}

DI_TEST_MAIN()
