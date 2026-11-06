// w3k_gate のテスト (1/4)。準備は w3k_gate_support.hpp。
#include "w3k_gate_support.hpp"

using namespace w3k_gate_support;

// ── golden ────────────────────────────────────────────────────────────────

TEST_CASE("golden: to_slug / derive_slug / glab_game_slug") {
    for (const auto& c : test::load_fixture("w3k/slugs.json")) {
        const std::string in = c["input"].get<std::string>();
        CHECK_EQ(ludus::to_slug(in), c["toSlug"].get<std::string>());
        CHECK_EQ(derive_slug(in), c["deriveSlug"].get<std::string>());
        if (c["glabGameSlug"].get<std::string>() != user_voices::glab_game_slug(in))
            throw test::Failure("glab_game_slug mismatch for " + in + ": " + user_voices::glab_game_slug(in));
    }
}

TEST_CASE("golden: is_auto_crawl_source / resolve_auto_crawl_sources (reddit の認証情報なし)") {
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_ID", std::nullopt);
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_SECRET", std::nullopt);
    const Json f = test::load_fixture("w3k/auto_crawl_sources.json");
    for (const auto& p : f["isAutoCrawlSource"]) CHECK_EQ(is_auto_crawl_source(p[0].get<std::string>()), p[1].get<bool>());
    for (const auto& r : f["resolve"]) {
        std::vector<std::string> sources;
        for (const auto& s : r["sources"]) sources.push_back(s.get<std::string>());
        const auto key = r["key"].is_null() ? std::optional<std::string>() : r["key"].get<std::string>();
        Json out = Json::array();
        for (const auto& s : resolve_auto_crawl_sources(sources, key)) out.push_back(s);
        CHECK_EQ(json_stringify(out), json_stringify(r["out"]));
    }
    // reddit は認証情報があれば自動経路に乗る
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_ID", "fake-id");
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_SECRET", "fake-secret");
    CHECK((resolve_auto_crawl_sources({"reddit", "steam"}) == std::vector<std::string>{"reddit"}));
    CHECK_EQ(reddit_credentials_from_env()->userAgent, std::string("LUDIARS-Discutere/0.1 (external discussion crawler)"));
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_ID", std::nullopt);
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_SECRET", std::nullopt);
    CHECK(!reddit_credentials_from_env());
}

TEST_CASE("golden: 情報密度評価のプロンプトと応答の解釈 (1 バイト単位)") {
    const Json cases = test::load_fixture("w3k/information_gate.json");
    CHECK(cases.size() == 40);
    for (const auto& c : cases) {
        ScriptLlm llm({c["response"].get<std::string>()});
        EvaluateArgs a;
        a.theme = "周回設計";
        a.tags = tags_from(c["tags"]);
        a.voices = voices_from(c["voices"]);
        a.llm = &llm;
        const auto ev = evaluate_information_density(a);
        CHECK_EQ(llm.last_system_, c["system"].get<std::string>());
        CHECK_EQ(llm.last_prompt_, c["prompt"].get<std::string>());
        CHECK_EQ(json_stringify(to_json(ev)), json_stringify(c["evaluation"]));
    }
}

TEST_CASE("golden: density_features") {
    for (const auto& c : test::load_fixture("w3k/density_features.json")) {
        const auto f = density_features(voices_from(c["voices"]), tags_from(c["tags"]));
        CHECK_EQ(json_stringify(f), json_stringify(c["features"]));
    }
}

// ── information-gate.test.ts ─────────────────────────────────────────────

TEST_CASE("is_density ガード") {
    CHECK(is_density(Json("rich")));
    CHECK(!is_density(Json("MODERATE")));
    CHECK(!is_density(Json(3)));
}

DI_TEST_MAIN()
