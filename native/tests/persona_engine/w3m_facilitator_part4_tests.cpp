// w3m_facilitator のテスト (4/4)。準備は w3m_facilitator_support.hpp。
#include "w3m_facilitator_support.hpp"

using namespace w3m_facilitator_support;

// ─── consensus scorer ───────────────────────────────────────────

TEST_CASE("parse_verdicts: 配列の切り出し・i は整数のみ・score は Number||0・壊れた入力と上限") {
    auto v = ps::parse_verdicts(R"(説明 [{"i":0,"agree":true,"score":4,"reasoning":"r"},{"i":"1","agree":"yes","score":"x"},{"i":1.5},{"x":1},5,[1]] 後書き)");
    CHECK(v.has_value());
    CHECK_EQ(v->size(), std::size_t(2));
    CHECK_EQ((*v)[0].i, 0.0);
    CHECK((*v)[0].agree);
    CHECK_EQ((*v)[0].score, 4.0);
    CHECK_EQ((*v)[0].reasoning.value_or(""), std::string("r"));
    CHECK_EQ((*v)[1].i, 1.0);
    CHECK(!(*v)[1].agree);
    CHECK_EQ((*v)[1].score, 0.0);
    CHECK(!ps::parse_verdicts("no array").has_value());
    CHECK(!ps::parse_verdicts("] [").has_value());
    CHECK(!ps::parse_verdicts("[{broken").has_value());
    CHECK(!ps::parse_verdicts(R"([1, 2] {"a": [}])").has_value());
    // 上限: ちょうど (1 MiB) は読める、+1 は拒否、深い入れ子は拒否
    const auto limit = ps::verdict_json_limits().max_bytes;
    std::string exact = "[\"" + std::string(limit - 4, 'a') + "\"]";
    CHECK_EQ(exact.size(), limit);
    CHECK(ps::parse_verdicts(exact).has_value());
    std::string over = "[\"" + std::string(limit - 3, 'a') + "\"]";
    CHECK(!ps::parse_verdicts(over).has_value());
    CHECK(!ps::parse_verdicts(str::repeat("[", 10'000) + str::repeat("]", 10'000)).has_value());
}

TEST_CASE("score_once: 未採点の意見 (persona/人間) を最大 8 件採点し、同意は opinion_scores に加点・👍 を付ける") {
    World w;
    std::vector<std::string> speakers{"persona:facilitator", "ext:yt-1", "persona:a", "user-1"};
    for (int i = 0; i < 8; ++i) speakers.push_back("persona:p" + std::to_string(i));
    const auto c = w.discussion("合意テスト", speakers, "discord:1/90");
    w.say(c.sessionId, "persona:a", "【収束】\nまとめ");
    discord_hook::ensure_reaction_tables(w.core->raw());
    // utterance 2 (persona:a) は Discord に投稿済み
    discord_hook::record_posted_message(w.core->raw(), {"m-1", c.utteranceIds[2], "utterance", std::string("ch-1"), std::nullopt});
    std::vector<std::string> prompts;
    MockResponder respond = [&](const LLMInvokeArgs& a) -> Variant {
        prompts.push_back(a.prompt);
        CHECK_EQ(a.maxTokens.value_or(0), 700);
        return std::string(R"([{"i":0,"agree":true,"score":4.5},{"i":1,"agree":false,"score":3},{"i":2,"agree":true,"score":9},{"i":99,"agree":true,"score":5},{"i":-1,"agree":true,"score":5}])");
    };
    auto llm = std::make_shared<MockLLMClient>(std::vector<MockResponder>{respond, respond});
    std::vector<ps::ConsensusReactInput> reacts;
    ps::ConsensusScorerDeps deps;
    deps.core = w.core;
    deps.llm = llm;
    deps.intervalMs = 1000;
    deps.logger = w.logger;
    deps.react = [&](const ps::ConsensusReactInput& r) {
        reacts.push_back(r);
        throw std::runtime_error("discord down");  // 失敗しても採点は続ける
    };
    auto scorer = ps::create_consensus_scorer(deps);
    scorer->score_once();

    CHECK_EQ(prompts.size(), std::size_t(1));
    // facilitator / ext: / 【収束】 は除外、posted_at 昇順で最大 8 件 → 0:persona:a 1:user-1 2..7:p0..p5 (【収束】は LIMIT の外)
    CHECK(str::contains(prompts[0], "0: 発言2"));
    CHECK(str::contains(prompts[0], "1: 発言3"));
    CHECK(!str::contains(prompts[0], "発言0"));
    CHECK(!str::contains(prompts[0], "発言1\n"));
    const auto scores = w.core->raw().all("SELECT utterance_id, agree, score FROM consensus_scores ORDER BY score DESC");
    CHECK_EQ(scores.size(), std::size_t(3));
    CHECK_EQ(w.core->raw().get("SELECT score FROM consensus_scores WHERE utterance_id = ?", {c.utteranceIds[2]})->i64("score"), 5);  // round(4.5)
    CHECK_EQ(w.core->raw().get("SELECT agree FROM consensus_scores WHERE utterance_id = ?", {c.utteranceIds[3]})->i64("agree"), 0);
    CHECK_EQ(discord_hook::get_opinion_score(w.core->raw(), c.utteranceIds[2]), 5);
    CHECK_EQ(discord_hook::get_opinion_score(w.core->raw(), c.utteranceIds[3]), 0);  // 不同意は加点しない
    CHECK_EQ(discord_hook::get_opinion_score(w.core->raw(), c.utteranceIds[4]), 5);  // 9 → 5 にクランプ
    CHECK_EQ(reacts.size(), std::size_t(1));  // mapping があるのは 1 件だけ
    CHECK_EQ(reacts[0].messageId, std::string("m-1"));
    CHECK_EQ(reacts[0].channelId, std::string("ch-1"));
    CHECK_EQ(reacts[0].emoji, std::string("👍"));
    CHECK(w.logger->has("consensus scorer react failed"));

    // 2 回目は採点済みを除いた残り
    scorer->score_once();
    CHECK_EQ(prompts.size(), std::size_t(2));
    CHECK(!str::contains(prompts[1], "発言2"));
}

TEST_CASE("score_once: LLM 失敗・壊れた応答は何も記録しない / closed の議論は対象外") {
    World w;
    const auto c = w.discussion("合意テスト 失敗", {"persona:a"}, "gap:z");
    const auto closed = w.discussion("合意テスト closed", {"persona:a"}, "gap:y");
    w.core->raw().run("UPDATE design_gaps SET status = 'closed' WHERE id = ?", {closed.gapId});
    int calls = 0;
    auto llm = std::make_shared<MockLLMClient>(std::vector<MockResponder>{
        [&](const LLMInvokeArgs&) -> Variant { ++calls; return LLMResult::failure("x"); },
        [&](const LLMInvokeArgs&) -> Variant { ++calls; return std::string("no verdict"); }});
    ps::ConsensusScorerDeps deps;
    deps.core = w.core;
    deps.llm = llm;
    deps.logger = w.logger;
    auto scorer = ps::create_consensus_scorer(deps);
    scorer->score_once();
    scorer->score_once();
    CHECK_EQ(calls, 2);
    CHECK_EQ(w.core->raw().get("SELECT COUNT(*) AS n FROM consensus_scores")->i64("n"), 0);
    CHECK(w.logger->has("consensus scorer llm failed"));
    CHECK(w.logger->has("consensus scorer parse failed"));
}

DI_TEST_MAIN()
