// 議題文 → 検索キーワード抽出 (tests/persona-engine/external-voices-lookup.test.ts の extractKeyTerms 節) + golden。
#include "core/test_util.hpp"

#include <algorithm>

#include "discatier_engine_adapter/keyword_terms.hpp"

using namespace di;
using namespace di::discatier_engine_adapter;

namespace {
bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
} // namespace

TEST_CASE("TS assertions") {
    auto t1 = extract_key_terms({"モンスターストライクの引っ張りアクションは面白いか"});
    CHECK(has(t1, "モンスターストライク"));
    CHECK(has(t1, "引っ張りアクション"));
    CHECK(!has(t1, "面白いか"));
    CHECK(extract_key_terms({"ボス戦"}) == std::vector<std::string>({"ボス戦"}));
    CHECK(extract_key_terms({""}).empty());
    CHECK(extract_key_terms({"の は が"}).empty());
    auto en = extract_key_terms({"Elden Ring の評価"});
    CHECK(has(en, "elden") && has(en, "ring") && has(en, "評価"));
    CHECK(extract_key_terms({"あ あ あ", "ボス戦 ボス戦"}).size() <= 6);
}

TEST_CASE("golden: extractKeyTerms matches Node") {
    const Json fx = test::load_fixture("discatier_engine_adapter/keyword_terms.json");
    for (const auto& c : fx) {
        std::vector<std::string> terms;
        for (const auto& t : c["terms"]) terms.push_back(t.get<std::string>());
        const Json actual = Json(extract_key_terms(terms, c["max"].get<std::size_t>()));
        if (actual != c["out"])
            throw test::Failure("terms " + json_stringify(c["terms"]) + "\n  actual:   " + json_stringify(actual) +
                                "\n  expected: " + json_stringify(c["out"]));
    }
}

TEST_CASE("boundaries: a 1MiB topic is split in linear time") {
    std::string big;
    for (int i = 0; i < 100000; ++i) big += "ガチャのと";
    auto out = extract_key_terms({big});
    CHECK(out.size() <= 6);
}

DI_TEST_MAIN()
