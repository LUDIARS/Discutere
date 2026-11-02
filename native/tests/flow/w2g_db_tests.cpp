// w2-G: DB を使う部品 (ペーパーの永続化・版履歴・声の保存と使い回し・議論一覧・削除・Discord の再議論・
// 弁証法 4 表・コストログ・games md) と、入力経路の境界 (上限ちょうど・上限+1・壊れた入力・巨大な入力)。
// TS の tests/flow/{paper-revisions,paper-reuse,foundation,cost-instrumentation}.test.ts の観点を移した。
#include "flow/w2g_support.hpp"

#include <chrono>

#include "base/strings.hpp"
#include "flow/cost_logger.hpp"
#include "flow/dialectic/store.hpp"
#include "flow/discord_flow_runs.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/effect_predict.hpp"
#include "flow/flow_setup.hpp"
#include "flow/games_md.hpp"
#include "flow/mechanic_extract.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/paper_reuse.hpp"
#include "flow/paper_revisions.hpp"
#include "flow/paper_voices.hpp"
#include "flow/spec_analyze.hpp"
#include "lib/front_matter/front_matter.hpp"

using namespace di;
using namespace di::flow;
using w2g::FakeLLM;

namespace {
NewPaper new_paper(const std::string& sid, const std::string& theme, std::optional<std::string> body = std::nullopt) {
    return {sid, theme, {"開発"}, {MechanicSummary{"M", "d", {}, {}, {}, {}, std::string("curated")}}, "補足", std::move(body)};
}
Json ok(const std::string& t) { return Json{{"ok", true}, {"text", t}}; }
} // namespace

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

TEST_CASE("discord_flow_runs: 再議論の草案は 1 つ・以前の行は残る") {
    w2g::TempFlowDb tmp;
    CHECK_EQ(forum_session_id("th1"), std::string("th1"));
    CHECK(!open_rediscussion("th1").has_value());
    persist_paper(new_paper("th1", "議題", std::string("# 議題\nX\n\n# 議論の経過\n古い")));
    const auto s = open_rediscussion("th1");
    CHECK(s.has_value());
    CHECK(str::starts_with(*s, "rediscussion-"));
    CHECK_EQ(forum_session_id("th1"), *s);
    CHECK_EQ(open_rediscussion("th1"), s);
    CHECK_EQ(get_draft_paper(*s)->bodyMd, std::string("# 議題\nX"));
    CHECK_EQ(latest_revision(*s)->origin, std::string("initial"));
    CHECK(get_paper_snapshot("th1").has_value());
    persist_paper(new_paper("th2", "壁打ち"), "sparring");
    CHECK(!open_rediscussion("th2").has_value());
}

TEST_CASE("dialectic store: 4 表の CRUD と読み出しの正規化") {
    w2g::TempFlowDb tmp;
    using namespace di::flow::dialectic;
    const auto i1 = insert_issue({"", "S", "論点2", 2, "paper-gate", "open"});
    insert_issue({"I0", "S", "論点1", 1, "llm", "open"});
    CHECK_EQ(i1.id.size(), std::size_t(36));
    update_issue_status(i1.id, "concluded");
    const auto issues = list_issues("S");
    CHECK_EQ(issues.size(), std::size_t(2));
    CHECK_EQ(issues[0].id, std::string("I0"));
    CHECK_EQ(issues[1].status, std::string("concluded"));
    CHECK_EQ(issues[1].source, std::string("paper-gate"));

    PositionRecord p;
    p.issueId = "I0";
    p.personaId = "p1";
    p.stance = "con";
    p.claim = "c";
    p.grounds = {{"con-G1", "d", "unchallenged", std::string("induction"), std::string("w"), {}, {}}};
    p.values = {"v"};
    const auto pr = insert_position(p);
    auto grounds = pr.grounds;
    grounds[0].state = "challenged";
    update_position_grounds(pr.id, grounds);
    auto ps = list_positions("I0");
    CHECK_EQ(ps.size(), std::size_t(1));
    CHECK_EQ(ps[0].grounds[0].state, std::string("challenged"));
    CHECK_EQ(*ps[0].grounds[0].kind, std::string("induction"));
    CHECK(!ps[0].qualifier.has_value());
    db::get_flow_db()->run("UPDATE flow_position SET grounds_json = 'bad', values_json = '{}', stance = 'x' WHERE id = ?", {pr.id});
    ps = list_positions("I0");
    CHECK(ps[0].grounds.empty());
    CHECK(ps[0].values.empty());
    CHECK_EQ(ps[0].stance, std::string("pro"));

    const auto t = insert_tension({"", "I0", "a", "b", "means", "open", std::nullopt});
    update_tension_status(t.id, "resolved", std::string("note"));
    update_tension_status(t.id, "compromised");
    auto ts = list_tensions("I0");
    CHECK_EQ(ts[0].status, std::string("compromised"));
    CHECK_EQ(*ts[0].resolutionNote, std::string("note"));
    update_tension_status(t.id, "weird", std::nullopt);
    db::get_flow_db()->run("UPDATE flow_tension SET type = 'zzz' WHERE id = ?", {t.id});
    ts = list_tensions("I0");
    CHECK_EQ(ts[0].status, std::string("open"));
    CHECK_EQ(ts[0].type, std::string("values"));
    CHECK(!ts[0].resolutionNote.has_value());

    SynthesisRecord s;
    s.tensionId = t.id;
    s.preserves = {"a"};
    s.kind = "aufheben";
    s.text = "x";
    s.ratification = {{"P", "accept", {"g"}, {}, "r"}};
    s.status = "revising";
    const auto sr = insert_synthesis(s);
    s.revision = 1;
    s.status = "ratified";
    update_synthesis(sr.id, s);
    const auto ss = list_syntheses(t.id);
    CHECK_EQ(ss.size(), std::size_t(1));
    CHECK_EQ(ss[0].revision, 1);
    CHECK_EQ(ss[0].status, std::string("ratified"));
    CHECK_EQ(ss[0].ratification[0].verdict, std::string("accept"));
    CHECK_EQ(ss[0].kind, std::string("aufheben"));
}

TEST_CASE("cost_logger: 1 呼び出し 1 行 (usage・推定コスト・失敗時は response NULL)") {
    w2g::TempFlowDb tmp;
    class UsageLLM : public persona_engine::LLMClient {
    public:
        persona_engine::LLMResult invoke(const persona_engine::LLMInvokeArgs& args) override {
            if (args.prompt == "fail") return persona_engine::LLMResult::failure("x");
            persona_engine::TokenUsage u;
            u.input_tokens = 1000;
            u.output_tokens = 500;
            u.model = "claude-sonnet-4-5";
            return persona_engine::LLMResult::success("resp", u);
        }
    } inner;
    auto logged = with_cost_log(inner, {"discussion", "CL", 2.0, 3.0, std::string("debater"), std::string("A"), "turn", std::string("test-backend")});
    persona_engine::LLMInvokeArgs a;
    a.prompt = "hello";
    a.model = "requested";
    CHECK(logged.invoke(a).ok);
    a.prompt = "fail";
    CHECK(!logged.invoke(a).ok);
    const auto rows = db::get_flow_db()->all("SELECT * FROM llm_call_log WHERE session_id = 'CL' ORDER BY id");
    CHECK_EQ(rows.size(), std::size_t(2));
    CHECK_EQ(rows[0].str("model"), std::string("claude-sonnet-4-5"));
    CHECK_EQ(rows[0].i64("round"), 2);
    CHECK_EQ(rows[0].str("backend"), std::string("test-backend"));
    CHECK_EQ(rows[0].i64("input_tokens"), 1000);
    CHECK(rows[0].opt_num("cost_usd").has_value());
    CHECK_EQ(rows[0].str("response"), std::string("resp"));
    CHECK(rows[1].is_null("response"));
    CHECK_EQ(rows[1].str("model"), std::string("requested"));
    CHECK(rows[1].is_null("input_tokens"));
}

TEST_CASE("games_md: upsert (新規・マージ・本文保持・パス検査・大きすぎるファイル)") {
    w2g::TempFlowDb tmp;
    const std::string dir = tmp.dir + "/games";
    CHECK_EQ(upsert_game_mechanics({dir, "g1", "ゲーム1", {}}), 0);
    GameMechanicEntry a{"A", std::string("説明"), {}, {}, std::vector<std::string>{"x"}, {}, std::string("crawl")};
    CHECK_EQ(upsert_game_mechanics({dir, "g1", "ゲーム1", {a, GameMechanicEntry{}}}), 1);
    auto text = fs::read_file(dir + "/g1.md");
    CHECK(text.has_value());
    auto parsed = lib::front_matter::parse(*text);
    CHECK_EQ(parsed->data["id"].get<std::string>(), std::string("g1"));
    CHECK(str::contains(parsed->content, "学習フローで収集したメカニクス。"));
    // 既存エントリへの重ね書き (他のキーは残る) と追加
    GameMechanicEntry a2{"A", {}, std::string("喜び"), {}, {}, {}, {}};
    GameMechanicEntry b{"B", std::string("b"), {}, {}, {}, {}, {}};
    CHECK_EQ(upsert_game_mechanics({dir, "g1", "無視", {a2, b}}), 2);
    parsed = lib::front_matter::parse(*fs::read_file(dir + "/g1.md"));
    const Json& ms = parsed->data["mechanics"];
    CHECK_EQ(ms.size(), std::size_t(2));
    CHECK_EQ(ms[0]["description"].get<std::string>(), std::string("説明"));
    CHECK_EQ(ms[0]["intended_affect"].get<std::string>(), std::string("喜び"));
    CHECK_EQ(ms[0]["source"].get<std::string>(), std::string("crawl"));
    CHECK_EQ(parsed->data["title"].get<std::string>(), std::string("ゲーム1"));
    // gamesDir の外を指す slug は拒否
    CHECK_THROWS(upsert_game_mechanics({dir, "../escape", "x", {b}}));
    CHECK(!fs::exists(tmp.dir + "/escape.md"));
    // 読めない front matter / 上限 (4MiB) を超えるファイルは上書きせずに例外
    fs::write_file_atomic(dir + "/bad.md", "---\na: &x 1\n---\n");
    CHECK_THROWS(upsert_game_mechanics({dir, "bad", "x", {b}}));
    fs::write_file_atomic(dir + "/big.md", std::string(kMaxGameFileBytes + 1, 'x'));
    CHECK_THROWS(upsert_game_mechanics({dir, "big", "x", {b}}));
    CHECK_EQ(fs::file_size(dir + "/big.md"), std::optional<std::uintmax_t>(kMaxGameFileBytes + 1));
    CHECK_EQ(mechanic_summary_to_entry({"n", "d", {}, {}, {}, {}, {}}).description, std::optional<std::string>("d"));
}

TEST_CASE("境界: LLM 応答 JSON の上限・壊れた入力・巨大な入力") {
    const std::size_t limit = llm_json_limits().max_bytes;
    // 上限ちょうど (= 読める) と上限+1 (= 読めない → 救済も上限で読めない)
    auto obj_of_size = [](std::size_t n) {
        std::string s = "{\"a\":\"";
        s += std::string(n - s.size() - 2, 'x');
        s += "\"}";
        return s;
    };
    CHECK(extract_json_object(obj_of_size(limit)).has_value());
    CHECK(!extract_json_object(obj_of_size(limit + 1)).has_value());
    // 入れ子が深すぎる応答は読まない (落ちない)
    std::string deep(100000, '[');
    CHECK(!extract_json_array(deep + std::string(100000, ']')).has_value());
    std::string deep_obj;
    for (int i = 0; i < 50000; ++i) deep_obj += "{\"a\":";
    CHECK(!extract_json_object(deep_obj).has_value());
    // 救済: 壊れた要素は捨て、残りを拾う。要素数が多くても線形で終わる
    std::string many = "[";
    for (int i = 0; i < 200000; ++i) many += "{\"name\":\"n" + std::to_string(i) + "\"},";
    many += "{\"name\": ";
    const auto t0 = std::chrono::steady_clock::now();
    const auto rescued = extract_json_array(many);
    CHECK(rescued.has_value());
    CHECK_EQ(rescued->size(), std::size_t(200000));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20));
    // 文字列中のブレース・エスケープを数えない
    CHECK_EQ((*extract_json_object("x {\"a\":\"}\\\"{\"} y"))["a"].get<std::string>(), std::string("}\"{"));
    CHECK(!extract_json_object("{\"a\": 1").has_value());
    CHECK(!extract_json_array("]]][[").has_value());
    // 巨大な応答でも mechanic 増補・仕様書解析は落ちずに degrade
    FakeLLM big(Json::array({ok("[" + std::string(8u << 20, ' ') + "{\"name\":\"X\"}]")}));
    EnrichMechanicsArgs ea;
    ea.target = 3;
    const auto enriched = enrich_mechanics(big, ea);
    CHECK_EQ(enriched.size(), std::size_t(1));
    FakeLLM garbage(Json::array({ok(std::string(1u << 20, '{'))}));
    AnalyzeSpecMechanicsArgs sa;
    sa.theme = "T";
    sa.specText = "spec";
    CHECK(analyze_spec_mechanics(garbage, sa).empty());
    // 仕様書のチャンク: 上限ちょうどは 1 チャンク、+1 で 2 チャンク、巨大でも最大 4
    CHECK_EQ(split_spec_into_chunks(std::string(16000, 'a')).size(), std::size_t(1));
    CHECK_EQ(split_spec_into_chunks(std::string(16001, 'a')).size(), std::size_t(2));
    CHECK_EQ(split_spec_into_chunks(std::string(10u << 20, 'a')).size(), std::size_t(4));
}

TEST_CASE("境界: markdown の巨大入力・壊れた入力でも線形に終わる") {
    std::string md = "# 議題\nT\n# ゲームのメカニクス\n";
    for (int i = 0; i < 20000; ++i) md += "- **" + std::string(50, '*') + "**: " + std::string(30, '*') + "\n";
    md += "# 論点\n";
    for (int i = 0; i < 20000; ++i) md += "1. 問い (両論: (((((( ( ( ( )\n";
    const auto t0 = std::chrono::steady_clock::now();
    PaperContent fb;
    fb.theme = "FB";
    const auto d = markdown_to_paper_draft(md, fb);
    CHECK_EQ(d.theme, std::string("T"));
    CHECK(d.issues.has_value());
    CHECK_EQ(d.issues->size(), std::size_t(20000));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30));
    // 壊れた UTF-8 でも落ちない
    CHECK_EQ(markdown_to_paper_draft(std::string("# 議題\n\xff\xfe\n# ゲームのメカニクス\n- **\xc3**: x"), fb).mechanics.size(), std::size_t(1));
    CHECK_EQ(strip_progress(std::string(1u << 20, ' ')), std::string());
}

TEST_CASE("flow_setup: 議論フローは調査せず、確定ペーパーは本文をそのまま使う") {
    w2g::TempFlowDb tmp;
    VoiceCache cache;
    FakeLLM llm(Json::array({ok("[]")}));
    FlowSetupArgs a;
    a.theme = "企画";
    a.tags = {"開発"};
    a.flow = "discussion";
    a.sessionId = "FS1";
    a.voiceCache = &cache;
    a.gamesDir = tmp.dir + "/no-games";
    const auto r = setup_flow_paper(llm, a);
    CHECK(r.paper.mechanics.empty());
    CHECK(!r.investigation.has_value());
    CHECK(llm.calls.empty());
    CHECK(str::contains(*r.paper.bodyMd, "# 議題\n企画"));
    CHECK_EQ(get_paper_snapshot("FS1")->status, std::string("started"));
    CHECK(str::starts_with(*get_paper_body_by_session("FS1"), "# 議題\n企画"));

    FlowSetupArgs b = a;
    b.sessionId = "FS2";
    b.flow = "improvement";
    b.paperOverride = PaperOverride{{MechanicSummary{"M", "d", {}, {}, {}, {}, {}}}, "補足", std::string("# 議題\n確定"), {}, {}};
    const auto r2 = setup_flow_paper(llm, b);
    CHECK_EQ(*get_paper_body_by_session("FS2"), std::string("# 議題\n確定"));
    CHECK(str::ends_with(*r2.paper.bodyMd, "# 議題\n確定"));
    CHECK_EQ(r2.paper.mechanics.size(), std::size_t(1));

    // 改善フロー: 調査 (games 無し) → 声が無いので増補しない
    FlowSetupArgs c = a;
    c.sessionId = "FS3";
    c.flow = "improvement";
    const auto r3 = setup_flow_paper(llm, c);
    CHECK(r3.investigation.has_value());
    CHECK(llm.calls.empty());
    FlowSetupArgs bad = a;
    bad.voiceCache = nullptr;
    CHECK_THROWS(setup_flow_paper(llm, bad));
}

int main() { return di::test::run_all(); }
