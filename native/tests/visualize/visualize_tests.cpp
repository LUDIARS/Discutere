// visualize: wikilink / data-sources / topic-opinions / learning-layers / locale_compare
// (tests/visualize/*.test.ts の移植 + golden fixtures/w1f/{wikilink,visualize}.json)。
#include "check.hpp"
#include "w1f_support.hpp"

#include <algorithm>
#include <filesystem>

#include "base/env.hpp"
#include "base/strings.hpp"
#include "visualize/data_sources.hpp"
#include "visualize/learning_layers.hpp"
#include "visualize/locale_compare.hpp"
#include "visualize/topic_opinions.hpp"
#include "visualize/wikilink.hpp"

using namespace di;
using namespace di::visualize;
using namespace di::test::w1f;

namespace {

Json opt_link(const std::optional<Wikilink>& l) { return l ? to_json(*l) : Json(nullptr); }

Json strip_generated(Json j) {
    j["generatedAt"] = 0;
    return j;
}

sql::DatabasePtr golden_db(const Json& fx) {
    auto db = sql::Database::open(":memory:");
    for (const auto& s : fx["schema"]) db->exec(s.get<std::string>());
    for (const auto& s : fx["inserts"]) db->exec(s.get<std::string>());
    return db;
}

std::string temp_vocabulary(const std::string& content) {
    const auto dir = std::filesystem::temp_directory_path() / ("w1f-vocab-" + std::to_string(env::pid()));
    std::filesystem::create_directories(dir);
    const std::string path = fs::from_path(dir / "vocabulary.json");
    fs::write_file_atomic(path, content);
    return path;
}

void check_json(const Json& got, const Json& want, const std::string& label) {
    if (json_stringify(got) != json_stringify(want))
        throw test::Failure(label + "\n  got:  " + json_stringify(got) + "\n  want: " + json_stringify(want));
}

} // namespace

TEST_CASE("wikilink parse / extract / format / normalize match Node") {
    const Json fx = load_fixture("wikilink.json");
    for (const auto& c : fx["parses"]) check_json(opt_link(parse_wikilink(c["input"].get<std::string>())), c["output"], "parse " + c["input"].get<std::string>());
    for (const auto& c : fx["extracts"]) {
        Json out = Json::array();
        for (const auto& l : extract_wikilinks(c["input"].get<std::string>())) out.push_back(to_json(l));
        check_json(out, c["output"], "extract " + c["input"].get<std::string>());
    }
    for (const auto& c : fx["formats"]) {
        Wikilink l{c["link"]["type"].get<std::string>(), c["link"]["id"].get<std::string>()};
        if (c.contains("error")) {
            try {
                format_wikilink(l);
                throw test::Failure("expected error");
            } catch (const std::invalid_argument& e) {
                CHECK_EQ(std::string(e.what()), c["error"].get<std::string>());
            }
        } else {
            CHECK_EQ(format_wikilink(l), c["output"].get<std::string>());
        }
    }
    for (const auto& c : fx["normalizes"]) {
        auto n = normalize_type(c["input"].get<std::string>());
        // TS は "constructor" 等で Object.prototype の関数を返し JSON から落ちる (C++ は null)。
        check_json(n ? Json(*n) : Json(nullptr), c.value("output", Json(nullptr)), "normalize " + c["input"].get<std::string>());
    }
    auto table = [](const std::vector<std::pair<std::string, std::string>>& t) {
        Json j = Json::object();
        for (const auto& [k, v] : t) j[k] = v;
        return j;
    };
    check_json(table(TYPE_TO_TABLE), fx["TYPE_TO_TABLE"], "TYPE_TO_TABLE");
    check_json(table(TYPE_TO_DIR), fx["TYPE_TO_DIR"], "TYPE_TO_DIR");
    check_json(table(FULL_TO_PREFIX), fx["FULL_TO_PREFIX"], "FULL_TO_PREFIX");
    Json types = Json::array();
    for (auto t : NODE_TYPES) types.push_back(std::string(t));
    check_json(types, fx["NODE_TYPES"], "NODE_TYPES");
}

TEST_CASE("wikilink extract is linear on huge / adversarial input") {
    const std::string adversarial = str::repeat("[[hyp:", 100000);
    CHECK(extract_wikilinks(adversarial).empty());
    const std::string many = str::repeat("[[utt:a]] ", 50000);
    CHECK_EQ(extract_wikilinks(many).size(), 50000u);
    CHECK(!parse_wikilink(std::string(1u << 20, '[')).has_value());
}

TEST_CASE("data source / topic / learning layer snapshots match Node") {
    const Json fx = load_fixture("visualize.json");
    auto db = golden_db(fx);
    check_json(build_data_source_snapshot(*db, "knowledge", 1000), fx["dataSources"], "dataSources");
    check_json(build_data_source_snapshot(*db, "nobody", 5), fx["dataSourcesOther"], "dataSourcesOther");
    check_json(strip_generated(build_topic_opinion_snapshot(*db, "knowledge")), fx["topics"], "topics");
    check_json(strip_generated(build_topic_opinion_snapshot(*db, "knowledge", {1.0, 1.0})), fx["topicsLimit"], "topicsLimit");
    check_json(strip_generated(build_topic_opinion_snapshot(*db, "knowledge", {1e9, -4.0})), fx["topicsHuge"], "topicsHuge");

    LearningLayerOptions base;
    base.vocabularyPath = temp_vocabulary(json_stringify(fx["vocabulary"]));
    const Json& layers = fx["layers"];
    check_json(strip_generated(build_learning_layer_snapshot(*db, "knowledge", "knowledge", base)), layers["knowledge"], "knowledge");
    auto o = base;
    o.limit = 2;
    check_json(strip_generated(build_learning_layer_snapshot(*db, "knowledge", "knowledge", o)), layers["knowledgeLimit"], "knowledgeLimit");
    check_json(strip_generated(build_learning_layer_snapshot(*db, "knowledge", "games", base)), layers["games"], "games");
    o = base;
    o.limit = 1.9;
    o.detailLimit = 0;
    check_json(strip_generated(build_learning_layer_snapshot(*db, "knowledge", "games", o)), layers["gamesLimit"], "gamesLimit");
    check_json(strip_generated(build_learning_layer_snapshot(*db, "knowledge", "opinions", base)), layers["opinions"], "opinions");
    o = base;
    o.limit = std::nan("");
    o.detailLimit = 1;
    check_json(strip_generated(build_learning_layer_snapshot(*db, "knowledge", "opinions", o)), layers["opinionsNaN"], "opinionsNaN");
    check_json(strip_generated(build_learning_layer_snapshot(*db, "other", "games", base)), layers["otherWs"], "otherWs");
    o = base;
    o.vocabularyPath = temp_vocabulary("{broken");
    check_json(strip_generated(build_learning_layer_snapshot(*db, "knowledge", "knowledge", o)), layers["knowledgeBroken"], "knowledgeBroken");

    const std::vector<std::optional<std::string>> inputs{"games", "bad", "knowledge", "opinions", "", "Games"};
    for (std::size_t i = 0; i < inputs.size(); ++i) CHECK_EQ(normalize_learning_layer(inputs[i]), fx["normalize"][i].get<std::string>());
    CHECK_EQ(normalize_learning_layer(std::nullopt), "knowledge");
}

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
    std::stable_sort(shuffled.begin(), shuffled.end(), [](const auto& a, const auto& b) { return locale_compare(a, b) < 0; });
    Json got = Json::array();
    for (const auto& n : shuffled) got.push_back(n);
    check_json(got, fx["sorted"], "localeCompare");
    CHECK_EQ(js_default_compare("b", "a"), 1);
    CHECK_EQ(js_default_compare("\xEF\xBD\x81", "😀"), 1);  // U+FF41 > サロゲート (0xD83D)
}

DI_TEST_MAIN()
