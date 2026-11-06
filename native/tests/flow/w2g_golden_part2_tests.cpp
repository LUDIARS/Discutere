// w2g_golden のテスト (2/4)。準備は w2g_golden_support.hpp。
#include "w2g_golden_support.hpp"

using namespace w2g_golden_support;

TEST_CASE("improvement_score: 射影スコアと勝者") {
    const Json& g = G("improvement");
    std::vector<double> gap = g["gap"].get<std::vector<double>>();
    auto mv = [](int k) {
        std::vector<double> v;
        for (int i = 0; i < 20; ++i) v.push_back(std::sin(i + k));
        return v;
    };
    std::vector<MovementCandidate> cands{{"a", mv(1), "effect-predict", std::vector<EffectChange>{{"asp.fun", 0.1, {}}}},
                                         {"b", mv(2), "lexicon-fallback", {}},
                                         {"c", mv(1), "effect-predict", {}}};
    const auto scored = score_opinions(cands, gap);
    // 入力の sin は Node と libm で最後の桁が違いうるので、スコアは誤差つきで比べる。
    for (std::size_t i = 0; i < scored.size(); ++i) {
        Json actual = to_json(scored[i]);
        Json expected = g["scored"][i];
        CHECK(std::abs(actual["score"].get<double>() - expected["score"].get<double>()) < 1e-12);
        actual.erase("score");
        expected.erase("score");
        W2G_SAME(actual, expected);
    }
    W2G_SAME(opt_json(pick_winner(scored)), g["winner"]);
    CHECK(!pick_winner({}).has_value());
    CHECK(std::abs(score_opinion(mv(3), mv(4), gap) - g["opinion"].get<double>()) < 1e-12);
}

TEST_CASE("mechanic_extract: enrichMechanics") {
    for (const auto& c : G("enrich")) {
        LlmCase lc(c);
        EnrichMechanicsArgs a;
        a.theme = "テーマ";
        a.existing = mechs_from(c["existing"]);
        a.voices.push_back({std::string(170 * 3, '\0'), std::string("s")});
        a.voices[0].content.clear();
        for (int i = 0; i < 170; ++i) a.voices[0].content += "い";
        for (int i = 0; i < 30; ++i) a.voices.push_back({"声" + std::to_string(i), std::nullopt});
        a.target = c["target"];
        a.model = "mm";
        a.warn = lc.warns.fn();
        lc.check(mechs_json(enrich_mechanics(lc.llm, a)), "enrich");
    }
}

TEST_CASE("spec_analyze: チャンク分割と抽出") {
    std::string long_spec;
    for (int i = 0; i < 12000; ++i) long_spec += "仕様😀";
    for (const auto& c : G("spec")) {
        LlmCase lc(c);
        AnalyzeSpecMechanicsArgs a;
        a.theme = "T";
        a.specText = c["specText"] == "LONG" ? long_spec : c["specText"].get<std::string>();
        if (c.contains("maxMechanics")) a.maxMechanics = c["maxMechanics"];
        a.warn = lc.warns.fn();
        const auto r = analyze_spec_mechanics(lc.llm, a);
        for (auto& call : lc.llm.calls) {
            const std::string prompt = call["prompt"];
            if (str::js_length(prompt) > 5000) call["prompt"] = "LEN:" + std::to_string(str::js_length(prompt));
        }
        lc.check(list_json(r), "spec");
    }
}

TEST_CASE("vote: parseVoteId / pickVoteWinner") {
    for (const auto& c : G("voteParse")) {
        const auto id = parse_vote_id(c["text"], w2g::strings(c["valid"]), w2g::strings(c["presented"]));
        W2G_SAME(opt_json(id), c["id"]);
    }
    for (const auto& c : G("voteWinner")) {
        std::vector<std::pair<std::string, double>> tally;
        for (auto it = c["tally"].begin(); it != c["tally"].end(); ++it) tally.emplace_back(it.key(), it.value().get<double>());
        const double r = c["r"];
        const auto w = pick_vote_winner(tally, [r] { return r; });
        W2G_SAME(opt_json(w.winner), c["result"]["winner"]);
        CHECK_EQ(w.maxVotes, c["result"]["maxVotes"].get<double>());
    }
}

TEST_CASE("voice_cache / mix / 保存した声の検索") {
    const auto& keys = G("voiceKeys");
    (void)keys;
    std::vector<std::string> seen_terms;
    auto cache = create_voice_cache([&](const std::vector<std::string>& terms, int limit) {
        return std::vector<ContextVoice>{{str::join(terms, "+") + std::to_string(limit), "s", std::nullopt}};
    });
    const Json& e = G("cache");
    W2G_SAME(w2g::voices_json(cache->lookup({"A", "b"}, 3)), e[0]);
    W2G_SAME(w2g::voices_json(cache->lookup({"b ", "a"}, 3)), e[1]);
    W2G_SAME(w2g::voices_json(cache->lookup({"a"}, 3)), e[2]);
    CHECK_EQ(cache->misses(), e[3].get<int>());
    CHECK_EQ(cache->hits(), e[4].get<int>());
    // JS の sort は UTF-16 順 (😀 は U+FF5A ｚ より前)。
    CHECK(voice_cache_key({"ｚ", "😀"}, 1) == voice_cache_key({"😀", "ｚ"}, 1));
    CHECK(voice_cache_key({"ｚ", "😀"}, 1) == std::string("1") + '\0' + "😀ｚ");

    const std::vector<ContextVoice> store{{"ガチャが楽しい", "s", {}}, {"周回がつらい", "s", {}}, {"ガチャと周回", "s2", "u"}};
    for (const auto& c : G("storedLookup"))
        W2G_SAME(w2g::voices_json(stored_voice_lookup(store)(w2g::strings(c["terms"]), c["limit"].get<int>())), c["result"]);
    auto mixed = user_voices::make_mixed_voice_lookup([&](const std::vector<std::string>&, int) {
        return std::vector<std::vector<ContextVoice>>{store, {store[1], {" 周回がつらい ", "x", {}}, {"別", "y", {}}}, {}};
    });
    W2G_SAME(w2g::voices_json(mixed({"x"}, 4)), G("mix"));
    CHECK(mixed({"x"}, 0).empty());
    const auto ev = user_voices::mix_evenly<int>({{1, 2, 3}, {1, 4}, {5}}, 10, [](int v) { return std::to_string(v); });
    W2G_SAME(Json(ev), G("mixEvenly"));
}

TEST_CASE("paper_reuse: 番号の読み取り") {
    for (const auto& c : G("reuseNo")) {
        const auto n = parse_reuse_paper_no(c[0].get<std::string>());
        W2G_SAME(n ? Json(*n) : Json(nullptr), c[1]);
    }
    CHECK(!parse_reuse_paper_no(std::nullopt).has_value());
}

int main() { return di::test::run_all(); }
