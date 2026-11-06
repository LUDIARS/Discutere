// w2g_db のテスト (1/3)。準備は w2g_db_support.hpp。
#include "w2g_db_support.hpp"

using namespace w2g_db_support;

TEST_CASE("paper_revisions: 追記・同一本文は no-op・戻すは前進") {
    w2g::TempFlowDb tmp;
    const std::string sid = "rev-s1";
    CHECK_EQ(append_revision({sid, "v1", "初期", "initial"}), 1);
    CHECK_EQ(append_revision({sid, "v2", "編集", "manual"}), 2);
    CHECK_EQ(latest_revision(sid)->bodyMd, std::string("v2"));
    CHECK_EQ(latest_revision(sid)->origin, std::string("manual"));
    CHECK_EQ(append_revision({sid, "v2", "同じ", "manual"}), 2);
    CHECK_EQ(list_revisions(sid).size(), std::size_t(2));
    CHECK(can_revert(sid));
    const auto reverted = revert_last(sid);
    CHECK(reverted.has_value());
    CHECK_EQ(reverted->bodyMd, std::string("v1"));
    CHECK_EQ(reverted->origin, std::string("revert"));
    CHECK_EQ(reverted->changeSummary, std::string("1 手前 (rev 1) に戻しました"));
    CHECK_EQ(list_revisions(sid).size(), std::size_t(3));
    CHECK(!can_revert("rev-none"));
    CHECK(!revert_last("rev-none").has_value());
    append_revision({"rev-one", "x", "", "initial"});
    CHECK(!revert_last("rev-one").has_value());
}

TEST_CASE("discussion_paper: draft → started の upsert・本文・review info・番号") {
    w2g::TempFlowDb tmp;
    const std::string sid = "dp-s1";
    const auto draft_id = persist_draft_paper(new_paper(sid, "下書き議題", std::string("# 議題\nT")));
    auto row = get_draft_paper(sid);
    CHECK(row.has_value());
    CHECK_EQ(row->paperId, draft_id);
    CHECK_EQ(row->flow, std::string("discussion"));
    CHECK(row->tags == std::vector<std::string>{"開発"});
    CHECK_EQ(row->mechanics.size(), std::size_t(1));
    CHECK_EQ(persist_draft_paper(new_paper(sid, "下書き議題(改)")), draft_id);
    CHECK_EQ(get_draft_paper(sid)->theme, std::string("下書き議題(改)"));
    const auto no = get_discussion_number_by_session(sid);
    CHECK(no.has_value());
    CHECK_EQ(persist_paper(new_paper(sid, "開始", std::string("# 議題\n開始"))), draft_id);
    CHECK(!get_draft_paper(sid).has_value());
    CHECK_EQ(get_discussion_number_by_session(sid), no);
    CHECK_EQ(get_paper_snapshot(sid)->status, std::string("started"));
    CHECK_EQ(*get_paper_body_by_session(sid), std::string("# 議題\n開始"));
    update_paper_body(draft_id, "# 議題\n開始\n\n# 議論の経過\nx");
    CHECK(str::contains(*get_paper_body_by_session(sid), "# 議論の経過"));
    CHECK(!get_paper_body_by_session("none").has_value());
    update_paper_derived(draft_id, "新補足", {});
    CHECK_EQ(get_paper_snapshot(sid)->supplement, std::string("新補足"));
    CHECK(get_paper_snapshot(sid)->mechanics.empty());
    CHECK(!has_flow_conclusion(sid));

    // review info / debatability
    CHECK(!get_paper_review_info(sid).has_value());
    set_paper_debatability(sid, Json{{"debatable", false}});
    W2G_SAME(*get_paper_review_info(sid), Json({{"voiceCount", 0}, {"countCapped", false}, {"samples", Json::array()}, {"debatability", {{"debatable", false}}}}));
    const Json info = {{"voiceCount", 3}, {"countCapped", true}, {"samples", {"a"}}, {"debatability", {{"debatable", true}}}};
    set_paper_review_info(sid, info);
    W2G_SAME(*get_paper_review_info(sid), info);
    auto stored = db::get_flow_db()->get("SELECT debatability_json FROM discussion_paper WHERE session_id = ?", {sid});
    CHECK_EQ(stored->str("debatability_json"), std::string("{\"debatable\":true}"));
    set_paper_review_info(sid, std::nullopt);
    CHECK(!get_paper_review_info(sid).has_value());
    // 壊れた JSON 列は null 扱い
    db::get_flow_db()->run("UPDATE discussion_paper SET review_info_json = '{broken', debatability_json = 'null' WHERE session_id = ?", {sid});
    CHECK(!get_paper_review_info(sid).has_value());
    db::get_flow_db()->run("UPDATE discussion_paper SET tags_json = 'xx', mechanics_json = '[1, {\"name\": 2}]' WHERE session_id = ?", {sid});
    CHECK(get_paper_snapshot(sid)->tags.empty());
    CHECK_EQ(get_paper_snapshot(sid)->mechanics.size(), std::size_t(2));

    // タイトルキャッシュ
    auto tc = db::get_flow_db()->get("SELECT title, discussion_type, origin_ui FROM discussion_title_cache WHERE session_id = ?", {sid});
    CHECK_EQ(tc->str("title"), std::string("開始"));
    CHECK_EQ(tc->str("origin_ui"), std::string("ai"));
    upsert_discussion_title_cache(sid, "【収束】 まとめ\n2", "conclusion");
    CHECK_EQ(db::get_flow_db()->get("SELECT title FROM discussion_title_cache WHERE session_id = ?", {sid})->str("title"), std::string("まとめ"));
    persist_draft_paper(new_paper("dp-chat", "壁打ち"), "sparring");
    CHECK_EQ(db::get_flow_db()->get("SELECT origin_ui FROM discussion_title_cache WHERE session_id = 'dp-chat'")->str("origin_ui"),
             std::string("chat"));
}

TEST_CASE("discussion_paper: 議論一覧の絞り込み・ページング・削除") {
    w2g::TempFlowDb tmp;
    persist_draft_paper(new_paper("ls-draft", "下書き"));
    persist_paper(new_paper("ls-live", "進行中"));
    persist_paper(new_paper("ls-done", "結論あり"));
    persist_draft_paper(new_paper("ls-chat", "壁打ち"), "sparring");
    auto db = db::get_flow_db();
    db->run("INSERT INTO flow_conclusion (session_id, paper_id, concluded, created_at) VALUES ('ls-done', 'x', 1, 1)");
    const auto all = list_flow_sessions({"all", "all", 100, 0});
    CHECK_EQ(all.total, 4);
    CHECK_EQ(list_flow_sessions({"draft", "all", 100, 0}).total, 2);
    CHECK_EQ(list_flow_sessions({"live", "all", 100, 0}).total, 1);
    const auto done = list_flow_sessions({"concluded", "all", 100, 0});
    CHECK_EQ(done.total, 1);
    CHECK_EQ(done.rows[0].sessionId, std::string("ls-done"));
    CHECK_EQ(done.rows[0].concluded, std::optional<std::int64_t>(1));
    CHECK_EQ(list_flow_sessions({"all", "chat", 100, 0}).total, 1);
    CHECK_EQ(list_flow_sessions({"all", "ai", 100, 0}).total, 3);
    const auto page = list_flow_sessions({"all", "all", 2, 1});
    CHECK_EQ(page.rows.size(), std::size_t(2));
    CHECK_EQ(page.total, 4);
    CHECK(to_json(page.rows[0]).contains("discussionNo"));
    CHECK_THROWS(list_flow_sessions({"bogus", "all", 10, 0}));
    CHECK_THROWS(list_flow_sessions({"all", "x; DROP TABLE vote", 10, 0}));

    CHECK(!delete_flow_session("none"));
    db->run("INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES ('ls-done', 1, 0, NULL, 1)");
    append_revision({"ls-done", "x", "", "initial"});
    CHECK(delete_flow_session("ls-done"));
    CHECK(!get_paper_snapshot("ls-done").has_value());
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM vote WHERE session_id='ls-done'")->i64("n"), 0);
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM flow_conclusion WHERE session_id='ls-done'")->i64("n"), 0);
    CHECK(list_revisions("ls-done").empty());
}

TEST_CASE("paper_voices / paper_reuse: 保存・上限・使い回し") {
    w2g::TempFlowDb tmp;
    const std::vector<ContextVoice> voices{{"天井が高すぎてつらい", "steam", std::string("https://example.com/1")},
                                           {"周回が楽しい", "youtube", {}},
                                           {"ガチャの天井は下げてほしい", "niconico", {}}};
    CHECK_EQ(save_paper_voices("s-voices", voices), std::size_t(3));
    CHECK(load_paper_voices("s-voices") == voices);
    CHECK_EQ(save_paper_voices("s-voices", {}), std::size_t(0));
    CHECK_EQ(load_paper_voices("s-voices").size(), std::size_t(3));
    save_paper_voices("s-voices", {voices[0]});
    CHECK_EQ(load_paper_voices("s-voices").size(), std::size_t(1));
    // 上限ちょうど・上限+1・空白だけの声は数えない
    std::vector<ContextVoice> many;
    for (std::size_t i = 0; i < PAPER_VOICE_LIMIT + 1; ++i) many.push_back({"声 " + std::to_string(i), "steam", {}});
    CHECK_EQ(save_paper_voices("s-many", many), PAPER_VOICE_LIMIT);
    many.resize(PAPER_VOICE_LIMIT);
    CHECK_EQ(save_paper_voices("s-exact", many), PAPER_VOICE_LIMIT);
    CHECK_EQ(save_paper_voices("s-blank", {{"   ", "s", {}}}), std::size_t(0));
    const auto lookup = stored_voice_lookup(voices);
    CHECK_EQ(lookup({"ガチャの天井"}, 3)[0].content, std::string("ガチャの天井は下げてほしい"));
    CHECK_EQ(lookup({"まったく関係ない語"}, 2).size(), std::size_t(2));
    CHECK(lookup({"天井"}, -1).empty());
    CHECK(static_cast<bool>(session_voice_lookup("s-voices")));
    CHECK(!static_cast<bool>(session_voice_lookup("s-none")));

    std::vector<MechanicSummary> mechanics{{"ガチャ天井", "200 連で確定", {}, {}, {}, {}, std::string("curated")}};
    PaperContent content;
    content.theme = "天井を下げるか";
    content.mechanics = mechanics;
    content.gameTitle = "テストゲーム";
    content.discussionTheme = "天井を下げるか";
    content.mechanicsContext = "ゲーム内容: 育成と周回のゲーム";
    const std::string body = paper_draft_to_markdown(content);
    persist_draft_paper({"s-src", "天井を下げるか", {}, mechanics, "", body});
    save_paper_voices("s-src", {{"天井が高すぎてつらい", "steam", {}}});
    const auto no = get_discussion_number_by_session("s-src");
    CHECK(no.has_value());
    const auto reused = load_reusable_paper(*no);
    CHECK(reused.has_value());
    CHECK_EQ(reused->sessionId, std::string("s-src"));
    CHECK_EQ(reused->mechanics[0].name, std::string("ガチャ天井"));
    CHECK(str::contains(reused->mechanicsContext, "育成と周回のゲーム"));
    CHECK_EQ(reused->voices.size(), std::size_t(1));
    CHECK(!load_reusable_paper(99999).has_value());
    CHECK_EQ(*merge_reused_context(reused, std::string("Notion 資料")), reused->mechanicsContext + "\n\nNotion 資料");
    CHECK_EQ(*merge_reused_context(reused, reused->mechanicsContext), reused->mechanicsContext);
    CHECK(!merge_reused_context(std::nullopt, std::nullopt).has_value());
    CHECK(str::contains(describe_reuse(*no, reused), "メカニクス 1 件・ゲーム内容・声 1 件 (集め直しません)"));
    CHECK(str::contains(describe_reuse(99999, std::nullopt), "見つからない"));
    auto no_voice = *reused;
    no_voice.voices.clear();
    CHECK(str::contains(describe_reuse(*no, no_voice), "保存が無いため通常どおり集めます"));
}

int main() { return di::test::run_all(); }
