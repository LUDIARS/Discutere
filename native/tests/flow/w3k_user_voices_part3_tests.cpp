// w3k_user_voices のテスト (3/3)。準備は w3k_user_voices_support.hpp。
#include "w3k_user_voices_support.hpp"

using namespace w3k_user_voices_support;

// ── listGameVoices (一時 KG) ────────────────────────────────────────────

TEST_CASE("list_game_voices: gameSlug で引き、議題ベクトルに近い順・未ベクトル化は後ろ (新しい順)") {
    test::TempDir dir;
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    auto attribution = cs::open_attribution_store(dir.file(".attribution.sqlite"));
    const std::string slug = steam_game_slug(7);
    std::vector<cs::ExternalUtterance> items;
    const std::vector<std::string> contents{"ガチャが渋い", "周回が楽しい", "グラフィックが綺麗"};
    for (std::size_t i = 0; i < contents.size(); ++i)
        items.push_back(utt("steam", "r" + std::to_string(i), slug, "7", contents[i], 1000.0 + static_cast<double>(i), "S" + std::to_string(i),
                            "https://example/r" + std::to_string(i)));
    items.push_back(utt("steam", "blank", slug, "7", "   ", 5000, "S9"));
    cs::ExternalImportOptions opts;
    opts.workspaceId = "knowledge";
    opts.attribution = attribution.get();
    cs::import_external_utterances(*core, items, opts);
    core->register_embedding({"knowledge", "utterance", "ext:steam:r0", {0, 1}});
    core->register_embedding({"knowledge", "utterance", "ext:steam:r1", {1, 0}});

    ListGameVoicesArgs a;
    a.core = core.get();
    a.attribution = attribution.get();
    a.workspaceId = "knowledge";
    a.gameSlug = slug;
    a.limit = 3;
    a.queryVector = std::vector<double>{1, 0.1};
    const auto ranked = list_game_voices(a);
    CHECK(ranked.size() == 3 && ranked[0].content == "周回が楽しい" && ranked[1].content == "ガチャが渋い" && ranked[2].content == "グラフィックが綺麗");
    CHECK(ranked[0].source == "steam" && ranked[0].sourceUrl == std::string("https://example/r1"));
    a.gameSlug = "steam-999";
    CHECK(list_game_voices(a).empty());
    a.gameSlug = slug;
    a.limit = 2;
    a.queryVector.reset();
    CHECK_EQ(list_game_voices(a).size(), std::size_t{2});
    a.limit = -1;
    CHECK(list_game_voices(a).empty());
    a.workspaceId = "other";
    a.limit = 10;
    CHECK(list_game_voices(a).empty());
    attribution->close();
    core->close();
}

// ── voice-index ──────────────────────────────────────────────────────────

TEST_CASE("voice_index: 未ベクトル化の声だけを埋める・2000 文字で切る・バッチ・ベクトル不足は失敗") {
    test::TempDir dir;
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    cs::ExternalImportOptions opts;
    opts.workspaceId = "knowledge";
    const std::string long_text = str::repeat("あ", 2500);
    cs::import_external_utterances(*core,
                                   {utt("steam", "a", "g", "t", long_text, 1, "A"), utt("steam", "b", "g", "t", "b", 2, "B"),
                                    utt("steam", "c", "g", "t", "c", 3, "C")},
                                   opts);
    core->register_embedding({"knowledge", "utterance", "ext:steam:c", {1, 1}});
    FakeEmbedder emb;
    core::vectors::EmbedVoiceOptions o;
    o.workspaceId = "knowledge";
    o.batchSize = 1;
    std::size_t progress = 0;
    o.onProgress = [&](std::size_t done, std::size_t) { progress = done; };
    const auto n = core::vectors::embed_voices_by_ids(*core, emb, {"ext:steam:a", "ext:steam:b", "ext:steam:a", "ext:steam:c", "missing"}, o);
    CHECK_EQ(n, std::size_t{2});
    CHECK_EQ(progress, std::size_t{2});
    CHECK_EQ(emb.batches.size(), std::size_t{2});
    CHECK_EQ(str::js_length(emb.batches[0][0]), core::vectors::EMBED_TEXT_CAP);
    CHECK_EQ(core->raw().get("SELECT COUNT(*) AS c FROM embeddings")->i64("c"), 3);
    // 既に埋まっていれば何もしない
    CHECK_EQ(core::vectors::embed_voices_by_ids(*core, emb, {"ext:steam:a"}, o), std::size_t{0});
    CHECK_EQ(core::vectors::embed_voices_by_ids(*core, emb, {}, o), std::size_t{0});
    // 空白だけの行は対象外
    CHECK_EQ(core::vectors::embed_voice_rows(*core, emb, {core::vectors::VoiceRow{"x", std::string("  ")}, core::vectors::VoiceRow{"y", std::nullopt}}, o),
             std::size_t{0});
    FakeEmbedder short_emb;
    short_emb.short_reply = true;
    o.batchSize = 0;  // 1 に丸める
    CHECK_THROWS(core::vectors::embed_voice_rows(*core, short_emb, {core::vectors::VoiceRow{"z", std::string("text")}}, o));
    core->close();
}

// ── prepare_user_voices ─────────────────────────────────────────────────

TEST_CASE("prepare_user_voices: 無効化・Core 未設定・全滅は lookup 無し / 集めた声をゲームごとに均等に混ぜる") {
    test::TempDir dir;
    config::LoadOptions lo;
    lo.env = std::map<std::string, std::string>{};
    lo.cwd = dir.file("");
    auto cfg = config::load_config(lo);
    cfg.embedding.enabled = false;
    cfg.flow.userVoices.enabled = true;

    const std::string kg = dir.file("db.kuzu");
    const std::string attr = dir.file(".attribution.sqlite");
    {
        auto core = core::create_core(kg, dir.file("events.jsonl"));
        auto attribution = cs::open_attribution_store(attr);
        cs::ExternalImportOptions opts;
        opts.workspaceId = "knowledge";
        opts.attribution = attribution.get();
        cs::import_external_utterances(*core,
                                       {utt("steam", "a1", "steam-7", "7", "A1", 1, "A"), utt("steam", "a2", "steam-7", "7", "A2", 2, "A"),
                                        utt("glab", "b1", glab_game_slug("別ゲーム"), "別ゲーム", "B1", 3, "anonymous")},
                                       opts);
        attribution->close();
        core->close();
    }
    PrepareUserVoicesArgs args;
    args.theme = "「新作」の操作感";
    args.similarGames = {"別ゲーム"};
    args.workspaceId = "knowledge";
    args.config = &cfg;
    args.openCore = [&] { return core::create_core(kg, dir.file("events.jsonl")); };
    args.openAttribution = [&] { return cs::open_attribution_store(attr); };
    args.baseLookup = [](const std::vector<std::string>&, int) { return std::vector<ContextVoice>{ContextVoice{"[synthetic] x", "llm", std::nullopt}}; };
    args.collectDeps = [](core::Core* c) {
        CollectUserVoicesDeps d;
        d.core = c;
        d.workspaceId = "knowledge";
        d.resolveSteam = [](const GameRef& g) {
            return g.title == "別ゲーム" ? std::optional<SteamApp>() : std::optional<SteamApp>(SteamApp{7, "新作", true});
        };
        d.fetchSteam = [](const SteamApp&, const std::string& slug) {
            return std::vector<cs::ExternalUtterance>{utt("steam", "a1", slug, "7", "A1", 1, "A")};
        };
        d.storedGlabCount = [](const std::string&) { return 1; };
        d.importItems = [](core::Core*, const std::vector<cs::ExternalUtterance>&, const std::string&) { return 0; };
        return d;
    };
    const auto prepared = prepare_user_voices(args);
    CHECK(prepared.reports.size() == 2 && prepared.reports[0].channel == "steam" && prepared.reports[1].channel == "glab");
    CHECK(static_cast<bool>(prepared.lookup));
    const auto mixed = prepared.lookup({"新作"}, 3);
    // 取込時刻が同じなので steam-7 の 2 件の順は問わない。ゲームごとに交互 (steam → glab → steam)。
    CHECK(mixed.size() == 3 && mixed[1].content == "B1");
    CHECK(((mixed[0].content == "A1" && mixed[2].content == "A2") || (mixed[0].content == "A2" && mixed[2].content == "A1")));

    auto disabled = cfg;
    disabled.flow.userVoices.enabled = false;
    auto a2 = args;
    a2.config = &disabled;
    CHECK(prepare_user_voices(a2).reports.empty());
    auto a3 = args;
    a3.openCore = nullptr;
    CHECK(!prepare_user_voices(a3).lookup);
    auto a4 = args;
    a4.collectDeps = [](core::Core*) { return CollectUserVoicesDeps{}; };
    const auto none = prepare_user_voices(a4);
    CHECK(none.reports.size() == 2 && !none.lookup);
    auto a5 = args;
    a5.includeThemeGame = false;
    a5.similarGames = {};
    CHECK(prepare_user_voices(a5).reports.empty());
}

DI_TEST_MAIN()
