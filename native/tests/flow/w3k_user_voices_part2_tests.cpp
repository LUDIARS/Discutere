// w3k_user_voices のテスト (2/3)。準備は w3k_user_voices_support.hpp。
#include "w3k_user_voices_support.hpp"

using namespace w3k_user_voices_support;

TEST_CASE("collect: 届いていた感想への fallback (Voluptas 停止 / 未設定 / 0 件)") {
    std::vector<std::vector<std::string>> embedded;
    const GameRef game{"テストゲーム", {"テストゲーム"}, "テストゲーム"};
    auto d = base_deps(&embedded);
    d.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(); };
    d.fetchGlab = [](const GameRef&, const std::string&) -> std::vector<cs::ExternalUtterance> { throw VoluptasUnavailable(); };
    d.storedGlabCount = [](const std::string& slug) { return slug == glab_game_slug("テストゲーム") ? 3 : 0; };
    const auto down = collect_game_voices(game, d);
    CHECK(down.channel == "glab" && down.collected == 3 && down.gameSlug == glab_game_slug("テストゲーム"));
    d.storedGlabCount = [](const std::string&) { return 0; };
    const auto down_none = collect_game_voices(game, d);
    CHECK(down_none.channel == "none" && down_none.voluptasDown.value_or(false));
    CHECK(str::contains(describe_voice_report(down_none), "Voluptas（Vo）が停止中"));

    auto nofetch = base_deps(&embedded);
    nofetch.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(); };
    nofetch.fetchGlab = nullptr;
    nofetch.storedGlabCount = [](const std::string&) { return 2; };
    CHECK_EQ(collect_game_voices(game, nofetch).channel, std::string("glab"));

    auto empty = base_deps(&embedded);
    empty.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(); };
    empty.fetchGlab = [](const GameRef&, const std::string&) { return std::vector<cs::ExternalUtterance>{}; };
    empty.storedGlabCount = [](const std::string&) { return 0; };
    CHECK_EQ(collect_game_voices(game, empty).channel, std::string("none"));
}

// ── importer (importer.test.ts / summary.test.ts) ────────────────────────

TEST_CASE("importer: Steam 風の取込・冪等・responds_to・2 層化・出所メタ") {
    test::TempDir dir;
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    auto ingested = cs::open_ingested_store(dir.file("ingested.sqlite"));
    cs::ExternalImportOptions opts;
    opts.workspaceId = "knowledge";
    opts.ingested = ingested.get();

    auto r1i = utt("steam", "r1", "vs", "1794680", "面白い", 1000, "S1", "https://example/r1");
    r1i.signal = cs::ExternalSignal{true, 5.0};
    auto r2i = utt("steam", "r2", "vs", "1794680", "重い", 2000, "S2", "https://example/r2");
    r2i.signal = cs::ExternalSignal{false, std::nullopt};
    const std::vector<cs::ExternalUtterance> steam{r1i, r2i};
    const auto r1 = cs::import_external_utterances(*core, steam, opts);
    CHECK(r1.sessions == 1 && r1.utterances == 2 && r1.reactions == 2 && r1.skipped == 0);
    const auto u1 = core->raw().get("SELECT * FROM utterances WHERE id = ?", {"ext:steam:r1"});
    CHECK(u1 && u1->str("speaker_id") == "ext:steam:S1" && u1->str("session_id") == "ext:steam:1794680" && u1->str("raw_content") == "面白い");
    const auto react = core->raw().get("SELECT * FROM reactions WHERE utterance_id = ?", {"ext:steam:r1"});
    CHECK(react && react->str("reaction_type") == "positive" && react->num("intensity") == 5);
    const auto session = core->raw().get("SELECT * FROM sessions WHERE id = ?", {"ext:steam:1794680"});
    CHECK(session && session->str("title") == "steam:vs");

    const auto r2 = cs::import_external_utterances(*core, steam, opts);
    CHECK(r2.utterances == 0 && r2.skipped == 2);
    CHECK_EQ(core->raw().get("SELECT COUNT(*) AS c FROM utterances")->i64("c"), 2);
    // sidecar 無しでも DB にあれば skip (sidecar に記録し直す)
    auto ingested2 = cs::open_ingested_store(dir.file("ingested2.sqlite"));
    opts.ingested = ingested2.get();
    const auto r2b = cs::import_external_utterances(*core, steam, opts);
    CHECK(r2b.utterances == 0 && r2b.skipped == 2 && ingested2->has("steam", "r1"));
    opts.ingested = ingested.get();

    const std::vector<cs::ExternalUtterance> threaded{
        [] {
            auto c = utt("reddit", "c2", "vs", "t1", "返信", 200, "U2");
            c.parentNativeId = "c1";
            return c;
        }(),
        utt("reddit", "c1", "vs", "t1", "親", 100, "U1")};
    const auto r3 = cs::import_external_utterances(*core, threaded, opts);
    CHECK_EQ(r3.utterances, 2.0);
    CHECK_EQ(core->raw().get("SELECT responds_to FROM utterances WHERE id = ?", {"ext:reddit:c2"})->str("responds_to"), std::string("ext:reddit:c1"));
    ingested->close();
    ingested2->close();

    // 2 層化 (summary → 本文、raw → raw-store)、出所メタ、タイトル生成
    auto raw = cs::open_raw_store(dir.file("raw.sqlite"));
    auto attribution = cs::open_attribution_store(dir.file("attr.sqlite"));
    const std::string long_text = str::repeat("ワンダと巨像のレビュー本文。", 60);
    auto lu = utt("website", "u-long", "sotc", "t1", long_text, 1, "famitsu.com", "https://famitsu.com/a");
    lu.summary = " 要約(長文) ";
    auto su = utt("website", "u-short", "sotc", "t2", "短い感想", 2, "blog.test", "https://blog.test/b");
    cs::ExternalImportOptions o2;
    o2.workspaceId = "knowledge";
    o2.rawStore = raw.get();
    o2.attribution = attribution.get();
    o2.sessionTitle = [](const cs::ExternalUtterance& i) { return "T:" + i.threadKey; };
    const auto rs = cs::import_external_utterances(*core, {lu, su}, o2);
    CHECK(rs.utterances == 2 && rs.summarized == 1);
    CHECK_EQ(core->raw().get("SELECT raw_content FROM utterances WHERE id = ?", {"ext:website:u-long"})->str("raw_content"), std::string("要約(長文)"));
    CHECK_EQ(core->raw().get("SELECT raw_content FROM utterances WHERE id = ?", {"ext:website:u-short"})->str("raw_content"), std::string("短い感想"));
    const auto doc = raw->get("ext:website:u-long");
    CHECK(doc && doc->rawText == long_text && doc->url == "https://famitsu.com/a");
    CHECK(!raw->get("ext:website:u-short"));
    const auto at = attribution->get("ext:website:u-long");
    CHECK(at && at->source == "website" && at->sourceUrl == "https://famitsu.com/a" && at->nativeId == "u-long" && at->gameSlug == std::string("sotc"));
    CHECK_EQ(core->raw().get("SELECT title FROM sessions WHERE id = ?", {"ext:website:t1"})->str("title"), std::string("T:t1"));
    // rawStore なしなら raw を本文に
    auto lu2 = lu;
    lu2.nativeId = "u-long2";
    cs::ExternalImportOptions o3;
    o3.workspaceId = "knowledge";
    const auto r4 = cs::import_external_utterances(*core, {lu2}, o3);
    CHECK_EQ(r4.summarized, 0.0);
    CHECK_EQ(core->raw().get("SELECT raw_content FROM utterances WHERE id = ?", {"ext:website:u-long2"})->str("raw_content"), long_text);
    raw->close();
    attribution->close();
    core->close();
}

TEST_CASE("importer: postedAt 昇順で入れる (session は最初の時刻)") {
    test::TempDir dir;
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    cs::ExternalImportOptions opts;
    opts.workspaceId = "knowledge";
    const auto r = cs::import_external_utterances(*core, {utt("reddit", "c", "g", "t", "c", 3, "C"), utt("reddit", "b", "g", "t", "b", 2, "B"), utt("reddit", "a", "g", "t", "a", 1, "A")}, opts);
    CHECK_EQ(r.utterances, 3.0);
    // session は最初に入った項目 (postedAt 最小) の時刻で作られる
    CHECK_EQ(core->raw().get("SELECT started_at FROM sessions WHERE id = ?", {"ext:reddit:t"})->num("started_at"), 1.0);
    core->close();
}

DI_TEST_MAIN()
