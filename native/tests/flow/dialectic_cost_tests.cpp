// トークンコスト最適化 (spec/plan/2026-11-23-token-cost-optimization.md) の受入。
#include "flow/w2g_support.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "flow/dialectic/exchange_prompt.hpp"
#include "flow/dialectic/position.hpp"
#include "flow/dialectic/position_stance.hpp"
#include "flow/dialectic/premise_rules.hpp"
#include "flow/dialectic/synthesis.hpp"
#include "flow/dialectic/toulmin.hpp"
#include "persona_engine/llm/claude_cli.hpp"

using namespace di;
using namespace di::flow;
using namespace di::flow::dialectic;

namespace {
Json ok(const std::string& text) { return Json{{"ok", true}, {"text", text}}; }
bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

PositionRecord position(const std::string& stance) {
    PositionRecord p;
    p.id = stance == "pro" ? "PA" : "PB";
    p.personaId = stance == "pro" ? "p-pro" : "p-con";
    p.stance = stance;
    p.claim = stance == "pro" ? "必要" : "不要";
    p.qualifier = "運営型";
    for (int i = 1; i <= 2; ++i) {
        Ground g;
        g.id = stance + "-G" + std::to_string(i);
        g.text = "データ" + std::to_string(i) + " とても長い説明が続く " + std::string(80, 'x');
        g.warrant = std::string("論拠") + std::to_string(i);
        g.backing = std::string("裏付け") + std::to_string(i);
        p.grounds.push_back(g);
    }
    return p;
}
} // namespace

TEST_CASE("コスト: 隔離した claude -p はツール・MCP・動的な system 節・上位の設定を読まない") {
    const auto plain = persona_engine::claude_cli_args(std::string("claude-sonnet-5-5"), false, false);
    CHECK(!has(plain, "--tools="));
    CHECK(!has(plain, "--exclude-dynamic-system-prompt-sections"));
    const auto iso = persona_engine::claude_cli_args(std::string("claude-sonnet-5-5"), false, true);
    CHECK(has(iso, "--tools="));
    CHECK(has(iso, "--strict-mcp-config"));
    CHECK(has(iso, "--disable-slash-commands"));
    CHECK(has(iso, "--exclude-dynamic-system-prompt-sections"));
    CHECK(has(iso, "--setting-sources"));
    CHECK(has(iso, "local"));
    // 会話専用の指定と重ねても --tools= は 1 回だけ。
    const auto both = persona_engine::claude_cli_args(std::nullopt, true, true);
    CHECK_EQ(std::count(both.begin(), both.end(), std::string("--tools=")), 1);
    // 既定の隔離フォルダは Ars の外 (LOCALAPPDATA か一時フォルダの下)。
    CHECK(persona_engine::default_claude_cli_home().find("llm-home") != std::string::npos);
}

TEST_CASE("コスト: 定立の answer の申告を読み、立場と合っているかを判定できる") {
    GeneratePositionArgs ga;
    ga.issue.ordinal = 1;
    ga.issue.title = "ガチャは必要か";
    ga.persona.name = "宮本";
    ga.stance = "pro";
    w2g::FakeLLM llm(Json::array({ok(R"({"answer":"YES","claim":"必要","qualifier":"運営型","force":"多くの場合",)"
                                      R"("grounds":[{"kind":"induction","data":"売上の 6 割","warrant":"運営に必要"}],"text":"必要"})")}));
    const auto gp = generate_position(llm, ga);
    CHECK_EQ(gp.declaredAnswer.value_or(""), std::string("yes"));
    CHECK(!is_reversed("pro", *gp.declaredAnswer));
    CHECK(is_reversed("con", *gp.declaredAnswer));
    // 指示文は前提ルールから始まる (先頭をそろえてキャッシュに当てる)。
    const std::string prompt = llm.calls[0]["prompt"].get<std::string>();
    CHECK(prompt.rfind(premise_rules_text(), 0) == 0);
    CHECK(prompt.find("\"answer\"") != std::string::npos);
}

TEST_CASE("コスト: 応酬の指示文は前提ルールから始まり、両陣営は要約で応答する根拠だけ詳しい") {
    ExchangePromptArgs pa;
    pa.persona.id = "p-con";
    pa.persona.name = "高瀬";
    pa.stance = "con";
    pa.issue.ordinal = 1;
    pa.issue.title = "ガチャは必要か";
    pa.positions = {position("pro"), position("con")};
    ArgumentNode root;
    root.id = "N1";
    root.side = "pro";
    root.personaId = "p-pro";
    root.groundId = std::string("pro-G1");
    root.data = "データ1";
    root.text = "say";
    pa.path = {root};
    const std::string prompt = build_exchange_prompt(pa);
    CHECK(prompt.rfind(premise_rules_text(), 0) == 0);
    CHECK(prompt.find("裏付け1") != std::string::npos);  // 応答する根拠 (pro-G1) は詳しく
    CHECK(prompt.find("論拠2") == std::string::npos);    // ほかの根拠は 1 行の要約
    CHECK(prompt.find("# あなた") > prompt.find("# 両陣営の主張と論拠"));
}

TEST_CASE("コスト: 要約は主張・限定・例外を出し、焦点の根拠以外を 1 行にする") {
    PositionRecord p = position("pro");
    p.exceptions = {"買い切り"};
    const std::string s = render_position_compact(p, "宮本", std::string("pro-G2"));
    CHECK(s.find("限定: 範囲: 運営型") != std::string::npos);
    CHECK(s.find("例外: 買い切り") != std::string::npos);
    CHECK(s.find("論拠2") != std::string::npos);
    CHECK(s.find("論拠1") == std::string::npos);
}

TEST_CASE("コスト: 折衷ゲートは gateModel (通常の判定モデル) で呼び、批准は judgeModel のまま") {
    w2g::TempFlowDb tmp;
    SynthesisLoopArgs sa;
    sa.issueTitle = "論点";
    sa.tension.id = "t1";
    sa.tension.type = "values";
    sa.positionA = position("pro");
    sa.positionB = position("con");
    w2g::FakeLLM gen(Json::array({ok(R"({"text":"案","preserves":["pro-G1","pro-G2","con-G1","con-G2"],"negates":[],"elevates":"上位目標で条件を分ける"})")}));
    w2g::FakeLLM gate(Json::array({ok("aufheben")}));
    w2g::FakeLLM ratify(Json::array({ok(R"({"verdict":"accept","preservedIds":["pro-G1","pro-G2"],"missing":[],"reason":"ok"})"),
                                     ok(R"({"verdict":"accept","preservedIds":["con-G1","con-G2"],"missing":[],"reason":"ok"})")}));
    sa.genLlm = &gen;
    sa.gateLlm = &gate;
    sa.ratifyLlm = &ratify;
    sa.judgeModel = std::string("final-judge");
    sa.gateModel = std::string("judge");
    const auto out = run_synthesis_loop(sa);
    CHECK_EQ(out.tensionStatus, std::string("synthesized"));
    CHECK_EQ(gate.calls[0]["model"].get<std::string>(), std::string("judge"));
    CHECK_EQ(ratify.calls[0]["model"].get<std::string>(), std::string("final-judge"));
}

DI_TEST_MAIN()
