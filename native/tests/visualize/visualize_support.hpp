#pragma once
// visualize_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// visualize: wikilink / data-sources / topic-opinions / learning-layers / locale_compare
// (tests/visualize/*.test.ts の移植 + golden fixtures/w1f/{wikilink,visualize}.json)。
#include "base/sort_by_index.hpp"
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

namespace visualize_support {

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

} // namespace visualize_support
