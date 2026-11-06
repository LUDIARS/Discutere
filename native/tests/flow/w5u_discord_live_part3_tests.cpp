// w5u_discord_live のテスト (3/4)。準備は w5u_discord_live_support.hpp。
#include "w5u_discord_live_support.hpp"

using namespace w5u_discord_live_support;

TEST_CASE("✅ 承認 / 破棄 / フロー再提案の続行") {
    w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_ENABLED", "true"}});
    reset_flow_live_for_test();
    Harness h;
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion();
    // 承認待ちが無ければ no-op (TS paper-review.test.ts と同じ)。
    CHECK(!handle_paper_review_approval("no-such-thread", deps));

    start_forum_flow(input("TC", "discussion"), deps);
    CHECK(has_paper_review("TC"));
    // 議論 → 改善 に選び直すと今のペーパーのまま続ける。
    CHECK(continue_paper_review_with_flow("TC", "improvement", deps));
    CHECK(str::starts_with(h.discord.post_contents("TC").back(), "👌 今のペーパーのまま「改善/議論」で続けます"));
    CHECK(!continue_paper_review_with_flow("TC", "sparring", deps));
    // 破棄すると draft 行も消え、返信は横取りされない。
    CHECK(cancel_paper_review("TC"));
    CHECK(!has_paper_review("TC"));
    CHECK(!get_draft_paper(forum_session_id("TC")).has_value());
    CHECK(!handle_paper_review_reply("TC", "G1", "開始", deps));

    // メモリから消えても永続ドラフトから再開でき、✅ で承認できる。
    start_forum_flow(input("TE", "discussion"), deps);
    reset_flow_live_for_test();  // 再起動相当 (draft 行は残る)
    CHECK(has_paper_review("TE"));
    CHECK(handle_paper_review_approval("TE", deps, {}, std::string("G1")));
    CHECK(w5u::drain(h.ex));
    CHECK_EQ(h.dispatched.size(), std::size_t{1});
    CHECK_EQ(h.dispatched[0].scene.value_or(""), std::string("discord:G1/TE"));
    CHECK(!has_paper_review("TE"));
}

TEST_CASE("途中で止まった議論のやり直し: 「再開」で確定ペーパーから・進行中は案内だけ・下書きや結論済みは対象外") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    Harness h;
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion();
    NewPaper p;
    p.sessionId = forum_session_id("TR2");
    p.theme = "周回の面白さ";
    p.bodyMd = "# 議題\n周回の面白さ\n";
    persist_paper(p, "discussion");
    CHECK(!handle_interrupted_discussion_reply("TR2", "G1", "こんにちは", deps));  // やり直しの語でない
    CHECK(handle_interrupted_discussion_reply("TR2", "G1", "再開", deps, h.hooks()));
    CHECK_EQ(h.dispatched.size(), std::size_t{1});
    CHECK(h.dispatched_deps[0].paperOverride && h.dispatched_deps[0].paperOverride->bodyMd);
    CHECK(str::starts_with(h.discord.post_contents("TR2")[0], "🔁 途中で止まった議論を"));
    CHECK(!handle_interrupted_discussion_reply("TR2-none", "G1", "再開", deps));  // ペーパー無し
}

TEST_CASE("壁打ち: 本物の dispatch は TS と同じく壁打ちを受理しない (エラーをスレッドに通知)") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    Harness h;
    start_forum_flow(input("TS0", "sparring"), h.deps(env.games()));
    CHECK(!has_sparring_session("TS0"));
    CHECK(str::starts_with(h.discord.post_contents("TS0").back(), "⚠️ フロー実行中にエラーが発生しました: "));
}

DI_TEST_MAIN()
