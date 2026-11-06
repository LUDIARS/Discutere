// w3k_user_voices のテスト (1/3)。準備は w3k_user_voices_support.hpp。
#include "w3k_user_voices_support.hpp"

using namespace w3k_user_voices_support;

// ── golden ───────────────────────────────────────────────────────────────

TEST_CASE("golden: theme_game_ref / parse_similar_games / describe_voice_report") {
    for (const auto& c : test::load_fixture("w3k/theme_game_refs.json")) {
        const auto ref = theme_game_ref(c["theme"].get<std::string>());
        Json terms = Json::array();
        for (const auto& t : ref.searchTerms) terms.push_back(t);
        CHECK_EQ(json_stringify(terms), json_stringify(c["ref"]["searchTerms"]));
        CHECK_EQ(ref.title, c["ref"]["title"].get<std::string>());
        CHECK_EQ(ref.matchAgainst, c["ref"]["matchAgainst"].get<std::string>());
    }
    for (const auto& c : test::load_fixture("w3k/similar_games.json")) {
        Json out = Json::array();
        for (const auto& t : parse_similar_games(c["text"].get<std::string>())) out.push_back(t);
        CHECK_EQ(json_stringify(out), json_stringify(c["out"]));
    }
    CHECK(parse_similar_games(std::nullopt).empty());
    for (const auto& c : test::load_fixture("w3k/voice_reports.json")) {
        const Json& j = c["report"];
        GameVoiceReport r;
        r.title = j["title"].get<std::string>();
        r.channel = j["channel"].get<std::string>();
        if (j["gameSlug"].is_string()) r.gameSlug = j["gameSlug"].get<std::string>();
        if (j.contains("appId")) r.appId = j["appId"].get<std::int64_t>();
        if (j.contains("appName")) r.appName = j["appName"].get<std::string>();
        r.unreleased = j.contains("unreleased") && j["unreleased"].get<bool>();
        r.collected = j["collected"].get<int>();
        r.imported = j["imported"].get<int>();
        r.embedded = j["embedded"].get<int>();
        if (j.contains("voluptasDown")) r.voluptasDown = j["voluptasDown"].get<bool>();
        CHECK_EQ(describe_voice_report(r), c["text"].get<std::string>());
        // to_json の値 (キー順は TS の spread に合わせてあるが、fixture は手書きの入力なので順序は問わない)
        CHECK_EQ(nlohmann::json::parse(json_stringify(to_json(r))), nlohmann::json::parse(json_stringify(j)));
    }
}

TEST_CASE("quoted_terms: 2〜40 文字の境界・入れ子・閉じ忘れ") {
    CHECK((quoted_terms("「ab」") == std::vector<std::string>{"ab"}));
    CHECK(quoted_terms("「a」").empty());
    CHECK((quoted_terms("「" + str::repeat("あ", 40) + "」") == std::vector<std::string>{str::repeat("あ", 40)}));
    CHECK(quoted_terms("「" + str::repeat("あ", 41) + "」").empty());
    CHECK((quoted_terms("「「ab」") == std::vector<std::string>{"「ab"}));
    CHECK(quoted_terms("「閉じない").empty());
    CHECK((quoted_terms("『ab」と「cd』") == std::vector<std::string>{"ab", "cd"}));
    CHECK((quoted_terms(str::repeat("「", 100000) + "x") .empty()));
}

// ── collect (user-voices.test.ts / impression-bridge.test.ts) ─────────────

TEST_CASE("collect: Steam / Di / YouTube / Voluptas / none の分岐 + ベクトル化 + 失敗時の継続") {
    std::vector<std::vector<std::string>> embedded;
    const GameRef game = theme_game_ref("「新作」の操作感");
    CHECK_EQ(game.searchTerms[0], std::string("新作"));

    auto d = base_deps(&embedded);
    d.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(SteamApp{7, "新作", true}); };
    const auto released = collect_game_voices(game, d);
    CHECK_EQ(released.channel, std::string("steam"));
    CHECK_EQ(released.gameSlug.value_or(""), steam_game_slug(7));
    CHECK((embedded.back() == std::vector<std::string>{"ext:steam:r1"}));

    int source_checks = 0;
    auto local = base_deps(&embedded);
    local.existingCount = [](const GameRef&) { return 3; };
    local.resolveSteam = [&](const GameRef&) {
        ++source_checks;
        return std::optional<SteamApp>();
    };
    const auto di_r = collect_game_voices(game, local);
    CHECK(di_r.channel == "di" && di_r.collected == 3 && source_checks == 0 && !di_r.gameSlug);

    auto yt = base_deps(&embedded);
    yt.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(SteamApp{7, "新作", true}); };
    yt.fetchSteam = [](const SteamApp&, const std::string&) { return std::vector<cs::ExternalUtterance>{}; };
    yt.fetchYoutube = [](const GameRef&, const std::string& slug) {
        return std::vector<cs::ExternalUtterance>{utt("youtube", "y1", slug, "video", "操作が楽しい", 1, "anonymous")};
    };
    const auto ytr = collect_game_voices(game, yt);
    CHECK_EQ(ytr.channel, std::string("youtube"));
    CHECK_EQ(ytr.gameSlug.value_or(""), "youtube-" + glab_game_slug(game.title));

    auto un = base_deps(&embedded);
    un.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(SteamApp{7, "新作", false}); };
    const auto unr = collect_game_voices(game, un);
    CHECK(unr.channel == "glab" && unr.unreleased);
    CHECK_EQ(unr.gameSlug.value_or(""), glab_game_slug(game.title));
    CHECK((embedded.back() == std::vector<std::string>{"ext:glab:v1"}));

    auto nf = base_deps(&embedded);
    nf.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(); };
    nf.fetchGlab = nullptr;
    const auto nfr = collect_game_voices(game, nf);
    CHECK(nfr.channel == "none" && !nfr.gameSlug && !nfr.voluptasDown);

    std::vector<std::string> warnings;
    auto failing = base_deps(&embedded);
    failing.resolveSteam = [](const GameRef&) -> std::optional<SteamApp> { throw std::runtime_error("network"); };
    failing.fetchGlab = [](const GameRef&, const std::string&) -> std::vector<cs::ExternalUtterance> { throw std::runtime_error("401"); };
    failing.warn = [&](const std::string& m) { warnings.push_back(m); };
    const auto fr = collect_game_voices(game, failing);
    CHECK(fr.channel == "none" && warnings.size() == 2);
    CHECK(fr.voluptasDown.has_value() && !*fr.voluptasDown);
    // ベクトル化の失敗は取込を止めない
    auto embfail = base_deps(&embedded);
    embfail.resolveSteam = [](const GameRef&) { return std::optional<SteamApp>(SteamApp{7, "新作", true}); };
    embfail.embed = [](core::Core*, const std::vector<std::string>&) -> int { throw std::runtime_error("embed down"); };
    std::vector<std::string> ew;
    embfail.warn = [&](const std::string& m) { ew.push_back(m); };
    const auto efr = collect_game_voices(game, embfail);
    CHECK(efr.channel == "steam" && efr.imported == 1 && efr.embedded == 0 && ew.size() == 1);
    // 複数ゲームは順に
    const auto all = collect_user_voices({game, similar_game_ref("別")}, d);
    CHECK(all.size() == 2 && all[1].title == "別");
}

DI_TEST_MAIN()
