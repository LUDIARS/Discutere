// src/flow/investigate.ts の移植の確認 (tests/flow/discussion.test.ts の investigateTheme の観点 + golden)。
#include "check.hpp"

#include <filesystem>

#include "base/crypto.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "flow/investigate.hpp"

using namespace di;
using namespace di::flow;

namespace {

struct TempDir {
    std::string path;
    TempDir() {
        path = fs::from_path(std::filesystem::temp_directory_path() / ("w1e-inv-" + crypto::uuid_v4()));
        fs::ensure_dir(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(fs::to_path(path), ec);
    }
};

Json fixture() {
    auto text = fs::read_file(test::fixtures_dir() + "/flow/w1e_investigate.json");
    if (!text) throw test::Failure("fixture missing");
    return parse_json_or_throw(*text);
}

} // namespace

TEST_CASE("loadThemeMechanics and sentiment counts match Node") {
    const Json fx = fixture();
    TempDir dir;
    for (auto it = fx["files"].begin(); it != fx["files"].end(); ++it)
        CHECK(fs::write_file_atomic(fs::join(dir.path, it.key()), it.value().get<std::string>()));
    for (const auto& c : fx["cases"]) {
        const std::string theme = c["theme"].get<std::string>();
        Json arr = Json::array();
        for (const auto& m : load_theme_mechanics(dir.path, theme)) arr.push_back(to_json(m));
        CHECK_EQ(json_stringify(arr), json_stringify(c["mechanics"]));
        InvestigateArgs a;
        a.theme = theme;
        a.gamesDir = dir.path;
        a.warn = [](const std::string&) {};
        CHECK_EQ(investigate_theme(a).sentimentCount, c["sentimentCount"].get<double>());
    }
}

TEST_CASE("YouTube fallback conditions (discussion.test.ts)") {
    int calls = 0;
    std::vector<std::string> warnings;
    InvestigateArgs a;
    a.theme = "テストテーマ";
    a.tags = {"開発"};
    a.gamesDir = "/nonexistent/no-games-dir";
    a.getSentimentCount = [](const std::string&) { return 0.0; };
    a.youtubeSearch = [&](const std::string&, int) {
        ++calls;
        return std::vector<std::string>{"コメント1", "コメント2"};
    };
    a.warn = [&](const std::string& m) { warnings.push_back(m); };
    auto r1 = investigate_theme(a);
    CHECK_EQ(calls, 1);
    CHECK(r1.youtubeUsed);
    CHECK_EQ(r1.youtubeComments.size(), std::size_t{2});
    CHECK(!warnings.empty() && warnings[0].find("YouTube") != std::string::npos);
    for (const char* tag : {"機密", "内部"}) {
        a.tags = {tag};
        investigate_theme(a);
        CHECK_EQ(calls, 1);
    }
    a.tags = {};
    a.getSentimentCount = [](const std::string&) { return 5.0; };
    investigate_theme(a);
    CHECK_EQ(calls, 1);
    a.getSentimentCount = [](const std::string&) { return 0.0; };
    a.youtubeSearch = [](const std::string&, int) -> std::vector<std::string> { throw std::runtime_error("quota"); };
    auto failed = investigate_theme(a);
    CHECK(!failed.youtubeUsed);
    CHECK(warnings.back().find("quota") != std::string::npos);
}

TEST_CASE("bounds: oversized and unreadable game files are skipped") {
    TempDir dir;
    // 上限ちょうどは読む、上限 + 1 は読まない
    std::string head = "---\nmechanics:\n  - name: 飛行\n---\n";
    CHECK(fs::write_file_atomic(fs::join(dir.path, "a.md"), head + std::string(kMaxGameFileBytes - head.size(), 'x')));
    CHECK(fs::write_file_atomic(fs::join(dir.path, "b.md"), "---\nmechanics:\n  - name: 飛行B\n---\n" + std::string(kMaxGameFileBytes, 'x')));
    CHECK(fs::write_file_atomic(fs::join(dir.path, "c.md"), "---\nmechanics: &a\n  - name: 飛行C\n---\n"));
    std::vector<std::string> warnings;
    const auto ms = load_theme_mechanics(dir.path, "飛行", [&](const std::string& m) { warnings.push_back(m); });
    CHECK_EQ(ms.size(), std::size_t{1});
    CHECK_EQ(ms[0].name, std::string("飛行"));
    CHECK_EQ(warnings.size(), std::size_t{2});
}

DI_TEST_MAIN()
