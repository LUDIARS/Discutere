// w2g_db のテスト (2/3)。準備は w2g_db_support.hpp。
#include "w2g_db_support.hpp"

using namespace w2g_db_support;

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

int main() { return di::test::run_all(); }
