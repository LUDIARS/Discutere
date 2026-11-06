// visualize のテスト (1/2)。準備は visualize_support.hpp。
#include "visualize_support.hpp"

using namespace visualize_support;

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

DI_TEST_MAIN()
