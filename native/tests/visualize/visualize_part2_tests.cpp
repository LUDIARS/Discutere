// visualize のテスト (2/2)。準備は visualize_support.hpp。
#include "visualize_support.hpp"

using namespace visualize_support;

TEST_CASE("vocabulary file limits: oversized / missing / deep files are ignored") {
    const Json fx = load_fixture("visualize.json");
    auto db = golden_db(fx);
    LearningLayerOptions o;
    o.vocabularyPath = temp_vocabulary(R"({"affects":[{"key":"a"}]})" + std::string(kAffectVocabularyMaxBytes, ' '));
    CHECK_EQ(build_learning_layer_snapshot(*db, "knowledge", "knowledge", o)["totals"]["affects"], 0);
    o.vocabularyPath = temp_vocabulary(R"({"affects":[{"key":"a"}]})");
    CHECK_EQ(build_learning_layer_snapshot(*db, "knowledge", "knowledge", o)["totals"]["affects"], 1);
    o.vocabularyPath = temp_vocabulary(R"({"affects":)" + str::repeat("[", 100) + str::repeat("]", 100) + "}");
    CHECK_EQ(build_learning_layer_snapshot(*db, "knowledge", "knowledge", o)["totals"]["affects"], 0);
    o.vocabularyPath = "/no/such/vocabulary.json";
    CHECK_EQ(build_learning_layer_snapshot(*db, "knowledge", "knowledge", o)["affects"].size(), 0u);
}

TEST_CASE("locale_compare sorts like Node's localeCompare for the golden set") {
    const Json fx = load_fixture("consensus.json");
    std::vector<std::string> names;
    for (const auto& n : fx["sorted"]) names.push_back(n.get<std::string>());
    std::vector<std::string> shuffled(names.rbegin(), names.rend());
    di::stable_sort_by_index(shuffled, [](const auto& a, const auto& b) { return locale_compare(a, b) < 0; });
    Json got = Json::array();
    for (const auto& n : shuffled) got.push_back(n);
    check_json(got, fx["sorted"], "localeCompare");
    CHECK_EQ(js_default_compare("b", "a"), 1);
    CHECK_EQ(js_default_compare("\xEF\xBD\x81", "😀"), 1);  // U+FF41 > サロゲート (0xD83D)
}

DI_TEST_MAIN()
