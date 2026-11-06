#pragma once
// w3l_conclusions_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w3-L: visualize の結論一覧・詳細・キャッシュ・md エクスポート・学習キャッシュのテスト。
//   - golden: tests/golden/w3-L-flow.gen.ts が記録した Node の出力と一致すること (md は 1 バイトも違わない)。
//   - tests/conclusion-cache/conclusion-cache.test.ts, tests/visualize/flow-conclusions.test.ts,
//     tests/visualize/conclusion-markdown.test.ts の観点。
//   - 境界: DB の JSON 列 (上限ちょうど・上限 + 1・壊れた・巨大)、Date の範囲外。
#include "check.hpp"
#include "flow/w3l_support.hpp"

#include <filesystem>

#include "base/sqlite.hpp"
#include "base/strings.hpp"
#include "core/index.hpp"
#include "flow/db/migrations.hpp"
#include "lib/front_matter/front_matter.hpp"
#include "visualize/conclusion_cache.hpp"
#include "visualize/conclusion_markdown.hpp"
#include "visualize/conclusions.hpp"
#include "visualize/flow_conclusions.hpp"
#include "visualize/learning_cache.hpp"

using namespace di;
using namespace di::visualize;
using w3l::Json;

namespace w3l_conclusions_support {

struct TempDir {
    TempDir() {
        path = (std::filesystem::temp_directory_path() / ("w3l-vis-" + crypto::uuid_v4())).string();
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string path;
};

Json summaries_json(const std::vector<ConclusionSummary>& list) {
    Json a = Json::array();
    for (const auto& s : list) a.push_back(to_json(s));
    return a;
}

Json opt_detail(const std::optional<ConclusionDetail>& d) { return d ? to_json(*d) : Json(nullptr); }

std::shared_ptr<sql::Database> flow_db(const std::string& path) {
    auto db = sql::Database::open(path);
    flow::db::apply_flow_migrations(*db);
    return db;
}

ConclusionSummary summary(const std::string& gap, const std::string& title, std::int64_t updated) {
    ConclusionSummary s;
    s.gapId = gap;
    s.kind = "flow";
    s.sessionId = gap.substr(5);
    s.title = title;
    s.conclusion = "達成感";
    s.utteranceCount = 18;
    s.aufhebungCount = 3;
    s.updatedAt = updated;
    return s;
}

class FakeAttribution : public crawler::sources::AttributionStore {
public:
    void set(const crawler::sources::AttributionInput&) override {}
    std::optional<crawler::sources::Attribution> get(const std::string& id) override {
        crawler::sources::Attribution a;
        a.utteranceId = id;
        if (id == "x2") {
            a.source = "youtube";
            a.sourceUrl = "https://youtu.be/abc";
            return a;
        }
        if (id == "x6") {
            a.source = "steam";
            a.sourceUrl = "";
            return a;
        }
        return std::nullopt;
    }
    std::vector<crawler::sources::Attribution> list_by_source(const std::string&, const std::optional<std::string>&, double) override { return {}; }
    std::vector<crawler::sources::SourceCount> counts_by_source() override { return {}; }
    void close() override {}
};

} // namespace w3l_conclusions_support
