// w4q_command_router のテスト (4/4)。準備は w4q_command_router_support.hpp。
#include "w4q_command_router_support.hpp"

using namespace w4q_command_router_support;

TEST_CASE("resolve_seed_in_background: 結果を渡す・満杯なら false で縮退") {
    std::vector<bool> got;
    resolve_seed_in_background([] { return true; }, [&got](bool s) { got.push_back(s); },
                               [](std::function<void()> fn) {
                                   fn();
                                   return true;
                               });
    resolve_seed_in_background([] { return true; }, [&got](bool s) { got.push_back(s); }, [](std::function<void()>) { return false; });
    resolve_seed_in_background(SeedTask{}, [&got](bool s) { got.push_back(s); });
    CHECK_EQ(got.size(), std::size_t{2});
    CHECK(got[0]);
    CHECK(!got[1]);
}

TEST_CASE("fire-and-forget: backup / persona-generate は背景で 1 回、満杯なら開始できない旨") {
    w4q::Env env;
    auto d = deps_with(env, {"admin1"});
    std::atomic<int> calls{0};
    std::optional<std::string> source;
    d.triggerBackup = [&calls] {
        ++calls;
        backup::BackupResult r;
        r.ok = true;
        r.bucket = "b";
        r.key = "k";
        return r;
    };
    d.triggerPersonaGenerate = [&](const std::optional<std::string>& s) {
        ++calls;
        source = s;
        return PersonaGenerateResult{2, 5};
    };
    CHECK(str::contains(route_slash_command(slash("discutere-backup"), d).content, "開始しました"));
    CHECK(str::contains(route_slash_command(slash("persona-generate", std::string("admin1"), "  steam "), d).content, "(source=steam)"));
    CHECK_EQ(source.value_or(""), std::string("steam"));
    CHECK_EQ(calls.load(), 2);
    d.submitBackground = [](std::function<void()>) { return false; };
    CHECK(str::contains(route_slash_command(slash("discutere-backup"), d).content, "開始できませんでした"));
    CHECK_EQ(calls.load(), 2);
    // trigger の例外は背景で握りつぶす
    d.submitBackground = [](std::function<void()> fn) {
        fn();
        return true;
    };
    d.triggerBackup = []() -> backup::BackupResult { throw std::runtime_error("s3 down"); };
    CHECK(str::contains(route_slash_command(slash("discutere-backup"), d).content, "開始しました"));
    // economy-graph
    d.triggerEconomyAnalysis = [](const std::string& t) { return EconomyAnalysisStarted{"slug-" + t, 4321}; };
    CHECK(str::contains(route_slash_command(slash("economy-graph", std::nullopt, " Slay "), d).content,
                        "http://localhost:4321/ludus/economy-graph/slug-Slay"));
    // queue の例外は文言にする
    d.buildQueueText = []() -> std::string { throw std::runtime_error("db"); };
    CHECK_EQ(route_slash_command(slash("discutere-queue"), d).content, std::string("⚠️ queue 取得失敗: db"));
}

TEST_CASE("整形: truncate_text (連続改行・UTF-16 の長さ) と ja_locale_string") {
    CHECK_EQ(truncate_text("a\n\n\nb\nc", 100), std::string("a\nb\nc"));
    CHECK_EQ(truncate_text("abcdef", 6), std::string("abcdef"));
    CHECK_EQ(truncate_text("abcdefg", 6), std::string("abcde…"));
    CHECK_EQ(truncate_text("あいう😀", 4), std::string("あいう…"));
    CHECK_EQ(truncate_text("", 3), std::string(""));
    w4q::Env env;  // TZ=UTC
    CHECK_EQ(ja_locale_string(1700000000000.0), std::string("2023/11/14 22:13:20"));
    CHECK_EQ(ja_locale_string(3000), std::string("1970/1/1 0:00:03"));
    CHECK_EQ(ja_locale_string(1e300), std::string("Invalid Date"));
}

DI_TEST_MAIN()
