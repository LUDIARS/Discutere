// トゥールミンモデルの 6 要素 (主張・データ・論拠・裏付け・限定 (範囲 + 確度)・反駁 = 例外条件) の受入
// (neco「トゥールミンモデルを正しく適用」2026-11-23、spec/feature/flow/dialectic.md §2.5)。
#include "flow/w2g_support.hpp"

#include "base/strings.hpp"

#include "flow/dialectic/drilldown.hpp"
#include "flow/dialectic/position.hpp"
#include "flow/dialectic/premise_rules.hpp"
#include "flow/dialectic/rule_check.hpp"
#include "flow/dialectic/store.hpp"
#include "flow/dialectic/toulmin.hpp"
#include "flow/dialectic/turn_prompt.hpp"

using namespace di;
using namespace di::flow;
using namespace di::flow::dialectic;

namespace {
Json ok(const std::string& text) { return Json{{"ok", true}, {"text", text}}; }
} // namespace

TEST_CASE("トゥールミン: 定立は裏付け・確度・例外条件を読み取り、保存して読み戻せる") {
    w2g::TempFlowDb tmp;
    GeneratePositionArgs ga;
    ga.theme = "T";
    ga.issue.ordinal = 1;
    ga.issue.title = "ガチャは必要か";
    ga.persona.name = "宮本";
    ga.stance = "pro";
    w2g::FakeLLM llm(Json::array({ok(R"({"claim":"ガチャは必要","qualifier":"運営型のスマホゲーム","force":"多くの場合",)"
                                      R"("exceptions":["買い切りのゲーム"],"grounds":[{"kind":"induction","data":"売上の 6 割",)"
                                      R"("warrant":"運営の継続に必要","backing":"長寿タイトルの決算","say":"売上の柱だよ"}],"text":"必要だと思う"})")}));
    const auto gp = generate_position(llm, ga);
    CHECK_EQ(gp.qualifierForce.value_or(""), std::string("多くの場合"));
    CHECK_EQ(gp.exceptions.size(), std::size_t{1});
    CHECK_EQ(gp.grounds[0].backing.value_or(""), std::string("長寿タイトルの決算"));
    CHECK(missing_position_elements(gp, false).empty());
    // 指示文は 6 要素を求める。
    const std::string prompt = llm.calls[0]["prompt"].get<std::string>();
    CHECK(prompt.find("\"force\"") != std::string::npos);
    CHECK(prompt.find("\"exceptions\"") != std::string::npos);
    CHECK(prompt.find("\"backing\"") != std::string::npos);

    IssueRecord issue;
    issue.sessionId = "S";
    issue.title = "ガチャは必要か";
    issue.ordinal = 1;
    issue = insert_issue(issue);
    PositionRecord pr;
    pr.issueId = issue.id;
    pr.personaId = "p";
    pr.claim = gp.claim;
    pr.qualifier = gp.qualifier;
    pr.qualifierForce = gp.qualifierForce;
    pr.exceptions = gp.exceptions;
    pr.grounds = gp.grounds;
    insert_position(pr);
    const auto back = list_positions(issue.id);
    CHECK_EQ(back.size(), std::size_t{1});
    CHECK_EQ(back[0].qualifierForce.value_or(""), std::string("多くの場合"));
    CHECK_EQ(back[0].exceptions.size(), std::size_t{1});
    CHECK_EQ(back[0].grounds[0].backing.value_or(""), std::string("長寿タイトルの決算"));
}

TEST_CASE("トゥールミン: データの言い換えの論拠は深掘りの対象、範囲・確度の欠けは対象外 (論拠の不足ではない)") {
    GeneratedPosition p;
    p.claim = "主張";
    Ground g;
    g.id = "pro-G1";
    g.text = "売上の 6 割がガチャ";
    g.warrant = "売上の 6 割がガチャ";
    g.kind = std::string("induction");
    p.grounds = {g};
    const auto missing = missing_position_elements(p, false);
    const std::string joined = str::join(missing, "|");
    CHECK(joined.find("言い換え") != std::string::npos);
    CHECK(joined.find("限定") == std::string::npos);
    CHECK_EQ(missing.size(), std::size_t{1});
}

TEST_CASE("トゥールミン: 突く先に裏付けと例外条件があり、例外条件・限定は根拠を指さなくてよい") {
    CHECK_EQ(coerce_attack_point(Json("backing")).value_or(""), std::string("backing"));
    CHECK_EQ(coerce_attack_point(Json("例外条件")).value_or(""), std::string("exception"));
    CHECK_EQ(attack_point_label("backing"), std::string("裏付け"));
    PositionRecord pos;
    pos.claim = "必要";
    Ground g;
    g.id = "pro-G1";
    g.text = "d";
    g.warrant = std::string("w");
    pos.grounds = {g};
    const auto ex = check_rebut_anchor({std::string("exception"), std::nullopt, std::string("d"), std::string("w")}, pos);
    CHECK(ex.ok);
    const auto bk_no_ground = check_rebut_anchor({std::string("backing"), std::nullopt, std::string("d"), std::string("w")}, pos);
    CHECK(!bk_no_ground.ok);
    CHECK_EQ(bk_no_ground.foul, std::string("unanchored"));
    const auto bk = check_rebut_anchor({std::string("backing"), std::string("pro-G1"), std::string("d"), std::string("w")}, pos);
    CHECK(bk.ok);
}

TEST_CASE("トゥールミン: 表示は限定を範囲と確度に分け、例外条件と裏付けを出す") {
    PositionRecord p;
    p.stance = "pro";
    p.claim = "必要";
    CHECK_EQ(render_qualifier(p), std::string("(明示なし)"));
    p.qualifier = "コア層";
    p.qualifierForce = "多くの場合";
    p.exceptions = {"買い切り"};
    CHECK_EQ(render_qualifier(p), std::string("範囲: コア層 / 確度: 多くの場合"));
    CHECK_EQ(render_exceptions(p), std::string("例外: 買い切り"));
    Ground g;
    g.id = "pro-G1";
    g.text = "d";
    g.warrant = std::string("w");
    g.backing = std::string("b");
    p.grounds = {g};
    const std::string digest = render_position_digest(p, "宮本");
    CHECK(digest.find("裏付け: b") != std::string::npos);
    CHECK(digest.find("確度: 多くの場合") != std::string::npos);
    CHECK(digest.find("例外: 買い切り") != std::string::npos);
    CHECK(premise_rules_text().find("反駁") != std::string::npos);
}

DI_TEST_MAIN()
