// w3k_gate のテスト (4/4)。準備は w3k_gate_support.hpp。
#include "w3k_gate_support.hpp"

using namespace w3k_gate_support;

TEST_CASE("autocrawl: 既定 collector の入力検査 (steam は appId、website は URL、youtube はキー、reddit は認証情報)") {
    const auto& d = default_collectors();
    CollectorContext ctx;
    ctx.theme = "t";
    ctx.gameSlug = "g";
    ctx.spec.source = "steam";
    CHECK_THROWS(d.at("steam")(ctx));
    ctx.spec.appId = 0;
    CHECK_THROWS(d.at("steam")(ctx));
    ctx.spec.source = "website";
    CHECK_THROWS(d.at("website")(ctx));
    ctx.spec.urls = std::vector<std::string>{};
    CHECK_THROWS(d.at("website")(ctx));
    ctx.spec.source = "youtube";
    CHECK_THROWS(d.at("youtube")(ctx));
    ctx.youtubeApiKey = "";
    CHECK_THROWS(d.at("youtube")(ctx));
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_ID", std::nullopt);
    ctx.spec.source = "reddit";
    CHECK_THROWS(d.at("reddit")(ctx));
    // website は利用者の URL: 私設アドレスは取得しない (SSRF 対策のまま)。失敗は skip されて 0 件。
    ctx.spec.source = "website";
    ctx.spec.urls = std::vector<std::string>{"http://127.0.0.1:1/x", "http://169.254.169.254/latest/meta-data/", "http://10.0.0.1/"};
    std::vector<cs::ExternalUtterance> got;
    try {
        got = d.at("website")(ctx);
    } catch (...) {
    }
    CHECK(got.empty());
}

TEST_CASE("collect_and_import: 未対応ソースは 0 件・collector の例外はそのまま投げる・0 件は取込を呼ばない") {
    CollectAndImportDeps d;
    d.theme = "t";
    d.spec.source = "bogus";
    std::vector<std::string> warns;
    d.warn = [&](const std::string& m) { warns.push_back(m); };
    const auto r = collect_and_import(d);
    CHECK(r.collected == 0 && r.imported == 0 && warns.size() == 1);
    d.spec.source = "niconico";
    d.collectors["niconico"] = [](const CollectorContext&) -> std::vector<cs::ExternalUtterance> { throw std::runtime_error("net"); };
    CHECK_THROWS(collect_and_import(d));
    bool import_called = false;
    d.collectors["niconico"] = [](const CollectorContext&) { return std::vector<cs::ExternalUtterance>{}; };
    d.importItems = [&](core::Core*, const std::vector<cs::ExternalUtterance>&, const std::string&) {
        import_called = true;
        return 0;
    };
    collect_and_import(d);
    CHECK(!import_called);
}

TEST_CASE("learning: 意見/メカニクス記録 + 極性・LLM を呼ばない・investigate が読める・個人データ非保管・日本語 slug・自動収集") {
    test::TempDir dir;
    const std::string games_dir = dir.file("games");
    std::filesystem::create_directories(games_dir);
    auto core = core::create_core(dir.file("db.sqlite"), dir.file("events.jsonl"));
    auto guard = std::make_shared<GuardSentiment>();
    crawler::sentiment::CascadeClients clients;
    clients.main = guard;

    LearningFlowDeps d;
    d.core = core.get();
    d.slug = "test-game";
    d.gamesDir = games_dir;
    d.sentimentClients = clients;
    d.opinions = {LearningOpinion{"このゲームは面白い、神ゲー", std::nullopt}, LearningOpinion{"つまらない、最悪だった", std::nullopt},
                  LearningOpinion{"   ", std::nullopt}};
    GameMechanicEntry m;
    m.name = "連鎖コンボ";
    m.description = "連鎖でスコアが伸びる";
    m.intended_affect = "爽快感";
    m.intended_valence = "positive";
    m.intended_aspects = std::vector<std::string>{"fun"};
    m.intended_emotions = std::vector<std::string>{"joy"};
    d.mechanics = {m};
    d.log = [](const std::string&) {};
    const auto result = run_learning_flow("Test Game", "Test Game の面白さ", d);
    CHECK_EQ(result.mechanicsRecorded, 1);
    CHECK_EQ(result.opinionsRecorded, 2);
    CHECK_EQ(result.polarityBreakdown["positive"].get<int>(), 1);
    CHECK_EQ(result.polarityBreakdown["negative"].get<int>(), 1);
    CHECK_EQ(json_stringify(result.polarityBreakdown), std::string(R"({"positive":1,"negative":1,"neutral":0})"));
    CHECK_EQ(guard->calls.load(), 0);

    InvestigateArgs ia;
    ia.theme = "Test Game";
    ia.gamesDir = games_dir;
    const auto inv = investigate_theme(ia);
    bool found = false;
    for (const auto& mm : inv.mechanics)
        if (mm.name == "連鎖コンボ") {
            found = true;
            CHECK_EQ(mm.intended_affect.value_or(""), std::string("爽快感"));
            CHECK((mm.intended_aspects == std::vector<std::string>{"fun"}));
        }
    CHECK(found);

    const auto rows = core->raw().all("SELECT id, speaker_id, raw_content FROM utterances WHERE speaker_id LIKE 'ext:feedback:%' ORDER BY posted_at");
    CHECK_EQ(rows.size(), std::size_t{2});
    for (const auto& r : rows) CHECK_EQ(r.str("speaker_id"), std::string("ext:feedback:anon"));
    bool has_kami = false;
    for (const auto& r : rows) has_kami = has_kami || str::contains(r.str("raw_content"), "神ゲー");
    CHECK(has_kami);
    for (const auto& c : core->raw().table_columns("utterances")) {
        const std::string lc = str::to_lower_ascii(c);
        CHECK(!str::contains(lc, "author") && !str::contains(lc, "name"));
    }
    // 賛否シグナル → reactions
    CHECK_EQ(core->raw().get("SELECT COUNT(*) AS c FROM reactions")->i64("c"), 2);

    // 日本語タイトル + slug 未指定
    LearningFlowDeps ja;
    ja.core = core.get();
    ja.gamesDir = games_dir;
    ja.opinions = {LearningOpinion{"モンストは引っ張りが楽しい、神ゲー", 1700000000000.0}};
    ja.log = [](const std::string&) {};
    const auto jr = run_learning_flow("モンスターストライク", "モンスターストライクの面白さ", ja);
    CHECK_EQ(jr.gameSlug, std::string("モンスターストライク"));
    CHECK_EQ(jr.opinionsRecorded, 1);
    CHECK(core->raw().get("SELECT 1 FROM utterances WHERE id = ?", {"ext:feedback:learning:モンスターストライク:1700000000000:0"}).has_value());

    // 自動収集モード
    std::vector<std::pair<std::string, std::string>> calls;
    LearningFlowDeps cr;
    cr.core = core.get();
    cr.slug = "test-game";
    cr.gamesDir = games_dir;
    cr.log = [](const std::string&) {};
    LearningCrawlSpec spec;
    spec.sources = {"niconico", "youtube"};
    spec.maxItems = 50;
    spec.youtubeApiKey = "dummy-key";
    spec.specBySource["niconico"] = LearningSourceParams{std::nullopt, std::nullopt, std::string("sub")};
    cr.crawl = spec;
    cr.collectAndImportFn = [&](const CollectAndImportDeps& c) {
        calls.emplace_back(c.spec.source, c.spec.query.value_or(""));
        CHECK_EQ(c.maxItems.value_or(0), 50);
        CHECK_EQ(c.slug, std::string("test-game"));
        if (c.spec.source == "niconico") CHECK_EQ(c.spec.subreddit.value_or(""), std::string("sub"));
        return CollectAndImportResult{3, c.spec.source == "niconico" ? 3 : 2};
    };
    const auto crr = run_learning_flow("Test Game", "Test Game の面白さ", cr);
    CHECK_EQ(crr.opinionsRecorded, 0);
    CHECK_EQ(crr.crawledImported, 5);
    CHECK_EQ(json_stringify(crr.crawledBySource), std::string(R"({"niconico":3,"youtube":2})"));
    CHECK(calls.size() == 2 && calls[0].first == "niconico" && calls[1].first == "youtube");
    CHECK(calls[0].second == "Test Game");

    // 1 ソース失敗は止めない
    LearningFlowDeps pa = cr;
    pa.crawl->query = " クエリ ";
    std::vector<std::string> warns;
    pa.warn = [&](const std::string& w) { warns.push_back(w); };
    pa.collectAndImportFn = [](const CollectAndImportDeps& c) -> CollectAndImportResult {
        if (c.spec.source == "youtube") throw std::runtime_error("youtube boom");
        CHECK_EQ(c.spec.query.value_or(""), std::string("クエリ"));
        return {4, 4};
    };
    const auto pr = run_learning_flow("Test Game", "Test Game の面白さ", pa);
    CHECK_EQ(pr.crawledImported, 4);
    CHECK_EQ(json_stringify(pr.crawledBySource), std::string(R"({"niconico":4})"));
    CHECK(warns.size() == 1 && warns[0] == "自動収集失敗 (source=youtube): youtube boom");
    core->close();
}

DI_TEST_MAIN()
