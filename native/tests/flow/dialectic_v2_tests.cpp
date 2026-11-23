// 議論エンジン v2 の受入 (spec/plan/2026-11-23-dialectic-v2-implementation-plan.md V1〜V8)。
// LLM は偽物 (w2g::FakeLLM)。DB を使う結論の保存は別のテスト (w4p) が通しで見る。
#include "flow/w2g_support.hpp"

#include "flow/dialectic/conclusion_summary.hpp"
#include "flow/dialectic/exchange.hpp"
#include "flow/dialectic/exchange_runner.hpp"
#include "flow/dialectic/goal.hpp"
#include "flow/dialectic/ground_state.hpp"
#include "flow/dialectic/persona_profile.hpp"
#include "flow/dialectic/scheduler.hpp"
#include "flow/dialectic/settlement_table.hpp"
#include "flow/dialectic/synthesis.hpp"
#include "persona_engine/llm/claude_cli.hpp"

using namespace di;
using namespace di::flow;
using namespace di::flow::dialectic;

namespace {

NewNode node(const std::string& side, const std::string& persona, const std::string& data,
             std::optional<std::string> ground_id = std::nullopt) {
    return NewNode{"u-" + data, side, persona, std::move(ground_id), data, {}, {}, "say " + data};
}

GroundState state_of(const GroundExchange& ex, const std::string& ground_id) {
    for (const auto& [id, s] : ex.ground_states())
        if (id == ground_id) return s;
    return "(none)";
}

Ground ground(const std::string& id, const std::string& state, const std::string& text = "data") {
    Ground g;
    g.id = id;
    g.text = text;
    g.state = state;
    return g;
}

PositionRecord position(const std::string& id, const std::string& stance, const std::string& claim, std::vector<Ground> grounds) {
    PositionRecord p;
    p.id = id;
    p.personaId = stance == "pro" ? "p-pro" : "p-con";
    p.stance = stance;
    p.claim = claim;
    p.grounds = std::move(grounds);
    return p;
}

Json ok(const std::string& text) { return Json{{"ok", true}, {"text", text}}; }

} // namespace

// ── V1: 未決を未決として扱う ─────────────────────────────────────────────

TEST_CASE("v2: 深さ上限の打ち切りは未決で、偶数・奇数どちらの深さでも持ち主の勝ちにならない") {
    for (int depth : {4, 5}) {
        GroundExchange ex({depth});
        const auto root = ex.add_opening(node("pro", "a", "g1", "pro-G1"));
        std::string target = root.id;
        // 反論を深さ上限まで積む (陣営を交互に)。
        for (int d = 1; d <= depth; ++d) {
            const std::string side = d % 2 == 1 ? "con" : "pro";
            target = ex.rebut(target, node(side, side == "pro" ? "a" : "b", "r" + std::to_string(d))).id;
        }
        CHECK_EQ(ex.verdict(root.id), std::string("contested"));
        CHECK_EQ(state_of(ex, "pro-G1"), std::string(GROUND_UNSETTLED));
        const auto t = ex.tally("pro");
        CHECK_EQ(t.unsettled, 1);
        CHECK_EQ(t.standing(), 0);
    }
}

TEST_CASE("v2: 応答者の LLM 障害は相手の反論を成立させず、根拠は未決になる") {
    GroundExchange ex({4});
    const auto root = ex.add_opening(node("pro", "a", "g1", "pro-G1"));
    const auto rebut = ex.rebut(root.id, node("con", "b", "r1"));
    ex.error(rebut.id);  // 賛成側が反論に答えられなかった
    CHECK_EQ(ex.verdict(rebut.id), std::string("contested"));
    CHECK_EQ(state_of(ex, "pro-G1"), std::string(GROUND_UNSETTLED));
    // 有効な反論にならなかった (問い) ときは従来どおり相手の論拠が成り立つ。
    GroundExchange ex2({4});
    const auto root2 = ex2.add_opening(node("pro", "a", "g1", "pro-G1"));
    const auto rebut2 = ex2.rebut(root2.id, node("con", "b", "r1"));
    ex2.fail(rebut2.id);
    CHECK_EQ(state_of(ex2, "pro-G1"), std::string(GROUND_CHALLENGED));
}

TEST_CASE("v2: 残った根拠が無く未決だけの陣営は論破にしない (結論なし)") {
    const auto a = position("A", "pro", "賛成の主張", {ground("pro-G1", "challenged"), ground("pro-G2", "unsettled")});
    const auto b = position("B", "con", "反対の主張", {ground("con-G1", "unchallenged")});
    const auto r = detect_refutation(a, b);
    CHECK(!r.note.has_value());
    CHECK(r.undecided.has_value());
    CHECK(r.undecided->find("pro-G2") != std::string::npos);
    CHECK(r.refuted.empty());
    // 全部崩れた側があれば従来どおり論破。
    const auto c = position("C", "pro", "賛成の主張", {ground("pro-G1", "challenged"), ground("pro-G2", "conceded")});
    const auto r2 = detect_refutation(c, b);
    CHECK(r2.note.has_value());
    CHECK_EQ(r2.refuted.size(), std::size_t{1});
    // 両方に残った根拠があれば決着は付けない (型分類へ)。
    const auto r3 = detect_refutation(b, position("D", "pro", "x", {ground("pro-G1", "defended")}));
    CHECK(!r3.note && !r3.undecided);
}

// ── V2: 打ち切りの告知・定型文 ───────────────────────────────────────────

TEST_CASE("v2: まとめ文は上限の打ち切りを『落ち着いた』と言わず内訳を告げる") {
    ExchangeEnd capped;
    capped.reason = "turn_cap";
    capped.answered = 16;
    capped.cutPending = 3;
    capped.errored = 1;
    GroundTally pro;
    pro.unchallenged = 1;
    pro.unsettled = 2;
    pro.total = 3;
    GroundTally con;
    con.broken = 1;
    con.total = 1;
    const std::string text = render_exchange_closing(capped, pro, con);
    CHECK(text.find("落ち着きました") == std::string::npos);
    CHECK(text.find("応答待ち 3 件") != std::string::npos);
    CHECK(text.find("応答できなかった 1 件") != std::string::npos);
    CHECK(text.find("未決 2") != std::string::npos);
    CHECK(text.find("崩れた 1") != std::string::npos);
    ExchangeEnd settled;
    CHECK(render_exchange_closing(settled, pro, con).find("落ち着きました") != std::string::npos);
}

TEST_CASE("v2: 指名文は LLM を通さない定型文で、引用の省略が分かる") {
    RenderNominationArgs na;
    na.decision.kind = "respond";
    na.personaName = "高瀬";
    na.target = UtteranceDigest{"u1", "宮本", std::string(200, 'x') + "。続き"};
    const std::string text = render_nomination_text(na);
    CHECK(text.find("高瀬さん") != std::string::npos);
    CHECK(text.find("…") != std::string::npos);
}

TEST_CASE("v2: 応答者はその連鎖で直近に話した同じ陣営の人を優先する (P6)") {
    FlowPersona a1;
    a1.id = "a1";
    a1.role = "debater";
    a1.stance = "pro";
    FlowPersona a2 = a1;
    a2.id = "a2";
    FlowPersona b1 = a1;
    b1.id = "b1";
    b1.stance = "con";
    const std::vector<FlowPersona> speakers{a1, a2, b1};
    GroundExchange ex({6});
    const auto root = ex.add_opening(node("pro", "a1", "g1", "pro-G1"));
    const auto r1 = ex.rebut(root.id, node("con", "b1", "r1"));
    const auto r2 = ex.rebut(r1.id, node("pro", "a2", "r2"));
    const auto r3 = ex.rebut(r2.id, node("con", "b1", "r3"));
    // a2 は発言数が多くても、この連鎖で直近に答えたので a2 が続ける。
    const std::map<std::string, int> counts{{"a1", 0}, {"a2", 5}, {"b1", 3}};
    const FlowPersona* picked = pick_chain_speaker(speakers, "pro", ex.path(r3.id), counts);
    CHECK(picked != nullptr);
    CHECK_EQ(picked->id, std::string("a2"));
    // 連鎖に陣営の発言者がいなければ発言数最少。
    const FlowPersona* fallback = pick_chain_speaker(speakers, "pro", ex.path(root.id), counts);
    CHECK_EQ(fallback->id, std::string("a1"));
}

// ── V3: 止揚と批准 ──────────────────────────────────────────────────────

TEST_CASE("v2: 止揚の候補が崩れた根拠を保存に挙げたら外して記録し、重複を除く") {
    GenerateSynthesisArgs ga;
    ga.issueTitle = "論点";
    ga.positionA = position("A", "pro", "賛成", {ground("pro-G1", "unchallenged"), ground("pro-G2", "conceded")});
    ga.positionB = position("B", "con", "反対", {ground("con-G1", "defended")});
    w2g::FakeLLM llm(Json::array(
        {ok(R"({"text":"統合案","preserves":["pro-G1","pro-G1","pro-G2","con-G1"],"negates":["pro-G2"],"elevates":"新しい枠"})")}));
    const auto out = generate_synthesis(llm, ga);
    CHECK_EQ(out.preserves.size(), std::size_t{2});
    CHECK_EQ(out.unusablePreserves.size(), std::size_t{1});
    CHECK_EQ(out.unusablePreserves[0], std::string("pro-G2"));
    // 崩れた根拠は「前提に使わない」枠で見せる。
    const std::string prompt = llm.calls[0]["prompt"].get<std::string>();
    CHECK(prompt.find("前提に使わない") != std::string::npos);
}

TEST_CASE("v2: 批准の分母は残った根拠だけで、同じ id の重複で過半数を満たせない") {
    RatifyArgs ra;
    ra.position = position("A", "pro", "賛成",
                           {ground("pro-G1", "unchallenged"), ground("pro-G2", "defended"), ground("pro-G3", "challenged")});
    ra.personaName = "宮本";
    ra.synthesisText = "統合案";
    // 残った根拠は 2 件。G1 を 2 回挙げても 1 件 = 過半数 (>1) に届かない。
    w2g::FakeLLM dup(Json::array({ok(R"({"verdict":"accept","preservedIds":["pro-G1","pro-G1"],"missing":[],"reason":"ok"})")}));
    CHECK_EQ(ratify_synthesis(dup, ra).verdict, std::string("reject"));
    // 崩れた G3 の保存を求めない (残った 2 件を確認できれば accept)。
    w2g::FakeLLM both(Json::array({ok(R"({"verdict":"accept","preservedIds":["pro-G1","pro-G2"],"missing":[],"reason":"ok"})")}));
    CHECK_EQ(ratify_synthesis(both, ra).verdict, std::string("accept"));
    CHECK(both.calls[0]["prompt"].get<std::string>().find("pro-G3") == std::string::npos);
    // 残った根拠が無ければ LLM を呼ばずに reject。
    RatifyArgs none = ra;
    none.position = position("A", "pro", "賛成", {ground("pro-G1", "challenged")});
    w2g::FakeLLM never(Json::array({ok("{}")}));
    CHECK_EQ(ratify_synthesis(never, none).verdict, std::string("reject"));
    CHECK_EQ(never.calls.size(), std::size_t{0});
}

TEST_CASE("v2: 崩れた根拠を保存に挙げた候補は折衷ゲートを呼ばずに止揚にならない") {
    SynthesisLoopArgs sa;
    sa.issueTitle = "論点";
    sa.tension.id = "t1";
    sa.tension.type = "values";
    sa.positionA = position("A", "pro", "賛成", {ground("pro-G1", "unchallenged"), ground("pro-G2", "conceded")});
    sa.positionB = position("B", "con", "反対", {ground("con-G1", "unchallenged")});
    w2g::FakeLLM gen(Json::array({ok(R"({"text":"案","preserves":["pro-G1","pro-G2","con-G1"],"negates":[],"elevates":"上位目標"})")}));
    w2g::FakeLLM gate(Json::array({ok("aufheben")}));
    w2g::FakeLLM ratify(Json::array({ok(R"({"verdict":"accept","preservedIds":["pro-G1","con-G1"],"missing":[],"reason":"ok"})")}));
    sa.genLlm = &gen;
    sa.gateLlm = &gate;
    sa.ratifyLlm = &ratify;
    w2g::TempFlowDb tmp;
    const auto out = run_synthesis_loop(sa);
    CHECK_EQ(out.tensionStatus, std::string("compromised"));
    CHECK_EQ(gate.calls.size(), std::size_t{0});
}

// ── V4: 決着表と結論 ────────────────────────────────────────────────────

TEST_CASE("v2: 決着表は全予定論点を載せ、未討議・崩れた根拠・打ち切りを書く") {
    IssueSettlement s;
    s.issue.ordinal = 1;
    s.issue.title = "論点一";
    s.positions = {position("A", "pro", "賛成の主張", {ground("pro-G1", "unchallenged", "残る"), ground("pro-G2", "conceded", "撤回")}),
                   position("B", "con", "反対の主張", {ground("con-G1", "unsettled", "決着せず")})};
    s.undecided = "決着しなかった";
    ExchangeEnd end;
    end.reason = "turn_cap";
    end.cutPending = 2;
    s.exchangeEnd = end;
    IssueRecord later;
    later.ordinal = 2;
    later.title = "論点二";
    SettlementTableArgs ta;
    ta.settlements = {s};
    ta.undiscussed = {later};
    ta.personaNames = {{"p-pro", "宮本"}, {"p-con", "高瀬"}};
    const std::string table = render_settlement_table(ta);
    CHECK(table.find("## 論点 1「論点一」【結論なし】") != std::string::npos);
    CHECK(table.find("## 論点 2「論点二」【未討議】") != std::string::npos);
    CHECK(table.find("崩れた根拠 (前提に使わない): [pro-G2]") != std::string::npos);
    CHECK(table.find("未決の根拠 (前提に使わない): [con-G1]") != std::string::npos);
    CHECK(table.find("応答待ち 2 件は未決") != std::string::npos);
    CHECK(table.find("宮本") != std::string::npos);
    CHECK(table.find("論点一") < table.find("論点二"));
}

TEST_CASE("v2: 結論の要約は決着表だけを入力にし、失敗しても決着表で結論になる") {
    DialecticSummaryArgs sa;
    sa.theme = "議題";
    sa.settlementTable = "## 論点 1「x」【論破】";
    const std::string prompt = build_dialectic_summary_prompt(sa);
    CHECK(prompt.find("## 論点 1「x」【論破】") != std::string::npos);
    CHECK(prompt.find("前提にも根拠にも使わない") != std::string::npos);
    CHECK(prompt.find("評価の高かった意見") == std::string::npos);
    w2g::FakeLLM broken(Json::array({ok("not json")}));
    CHECK(!generate_dialectic_summary(broken, sa).has_value());
    CHECK_EQ(broken.calls.size(), std::size_t{2});  // JSON 崩れは 1 回だけやり直す
    w2g::FakeLLM good(Json::array({ok(R"({"summary":"要約です"})")}));
    CHECK_EQ(generate_dialectic_summary(good, sa).value_or(""), std::string("要約です"));
    const std::string body = compose_dialectic_conclusion(std::nullopt, sa.settlementTable, {"事実 A は未確認"});
    CHECK(body.find("要約は作れなかった") != std::string::npos);
    CHECK(body.find("# 論点ごとの決着") != std::string::npos);
    CHECK(body.find("事実 A は未確認") != std::string::npos);
}

// ── V7 / V8 ─────────────────────────────────────────────────────────────

TEST_CASE("v2: 遊び手の型の行はプール人物だけに出て、代理には出さない") {
    FlowPersona plain;
    CHECK(persona_profile_line(plain).empty());
    FlowPersona pooled;
    pooled.playerProfile = "大きな体験と探索を求める";
    CHECK(persona_profile_line(pooled).find("大きな体験と探索を求める") != std::string::npos);
    FlowPersona possessed;
    possessed.possession = PersonaPossession{"論者#1", std::string("収集を好む")};
    CHECK(persona_profile_line(possessed).find("収集を好む") != std::string::npos);
    FlowPersona proxy = pooled;
    proxy.proxy = ProxyPersonaContext{};
    CHECK(persona_profile_line(proxy).empty());
}

TEST_CASE("v2: claude CLI の異常終了で stderr が空なら stdout の先頭を残す (P11)") {
    CHECK_EQ(persona_engine::claude_cli_exit_error(1, "boom", "out"), std::string("claude cli exit 1: boom"));
    CHECK(persona_engine::claude_cli_exit_error(1, "  ", "rate limited").find("stdout: rate limited") != std::string::npos);
    CHECK(persona_engine::claude_cli_exit_error(2, "", "").find("出力なし") != std::string::npos);
}

DI_TEST_MAIN()
