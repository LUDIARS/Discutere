// 論拠の形を成していない発言の深掘り (spec/plan/2026-11-23-dialectic-drilldown.md) の受入。
#include "flow/w2g_support.hpp"

#include <memory>

#include "flow/dialectic/drilldown.hpp"
#include "flow/dialectic/exchange.hpp"
#include "flow/dialectic/exchange_runner.hpp"

using namespace di;
using namespace di::flow;
using namespace di::flow::dialectic;

namespace {

Json ok(const std::string& text) { return Json{{"ok", true}, {"text", text}}; }

ParsedExchangeTurn rebut(std::optional<std::string> data, std::optional<std::string> warrant, std::optional<std::string> attack) {
    ParsedExchangeTurn t;
    t.act = "rebut";
    t.data = std::move(data);
    t.warrant = std::move(warrant);
    t.attack = std::move(attack);
    t.text = "反論です";
    return t;
}

RebutGateResult gate(bool accepted, const std::string& foul) {
    RebutGateResult g;
    g.accepted = accepted;
    g.foul = foul;
    return g;
}

/** 決まった順の応答を返す偽の LLM (w2g::FakeLLM) を毎回同じ実体で渡すための薄い包み。 */
class Borrowed : public persona_engine::LLMClient {
public:
    explicit Borrowed(persona_engine::LLMClient& inner) : inner_(inner) {}
    persona_engine::LLMResult invoke(const persona_engine::LLMInvokeArgs& a) override { return inner_.invoke(a); }

private:
    persona_engine::LLMClient& inner_;
};

struct Run {
    std::vector<FlowUtteranceRecord> records;
    ExchangeEnd end;
    GroundState state;
    std::size_t turnCalls = 0;
};

/** 賛成の論拠 1 件に反対の 1 人が答える応酬を回す。turn は反対の応答の LLM 応答列。 */
Run run_one(const Json& turn_responses, bool drill) {
    w2g::TempFlowDb tmp;
    FlowPersona pro;
    pro.id = "p-pro";
    pro.name = "宮本";
    pro.role = "debater";
    pro.stance = "pro";
    FlowPersona con = pro;
    con.id = "p-con";
    con.name = "高瀬";
    con.stance = "con";
    FlowPersona fac = pro;
    fac.id = "p-fac";
    fac.name = "進行役";
    fac.role = "facilitator";
    fac.stance = "neutral";

    PositionRecord pos;
    pos.id = "PA";
    pos.personaId = pro.id;
    pos.stance = "pro";
    pos.claim = "ガチャは必要";
    Ground g;
    g.id = "pro-G1";
    g.text = "売上の 6 割がガチャ";
    g.warrant = "運営の継続に必要";
    pos.grounds = {g};
    PositionRecord con_pos = pos;
    con_pos.id = "PB";
    con_pos.personaId = con.id;
    con_pos.stance = "con";
    con_pos.claim = "ガチャは不要";
    PositionsBySide by_side;
    by_side.set("pro", &pos);
    by_side.set("con", &con_pos);

    GroundExchange ex({4});
    NewNode opening{"u-open", "pro", pro.id, std::string("pro-G1"), g.text, g.warrant, {}, "say"};
    ex.add_opening(opening);

    w2g::FakeLLM turn_llm(turn_responses);
    w2g::FakeLLM judge(Json::array({ok("ok")}));
    std::map<std::string, UtteranceDigest> index{{"u-open", {"u-open", pro.name, "say"}}};
    std::map<std::string, int> counts;
    Run out;
    int seq = 0;

    GroundExchangeArgs a;
    a.issue.ordinal = 1;
    a.issue.title = "ガチャは必要か";
    a.round = 1;
    a.exchange = &ex;
    a.positionBySide = &by_side;
    a.speakers = {pro, con};
    a.debaters = Debaters{pro, con};
    a.facilitator = fac;
    a.utteranceIndex = &index;
    a.speakCounts = &counts;
    a.turnCap = 4;
    a.ruleCheck = false;
    a.drillDown = drill;
    a.turnLlm = [&](const FlowPersona&, int) { return std::make_unique<Borrowed>(turn_llm); };
    a.ruleCheckLlm = [&](int) { return std::make_unique<Borrowed>(judge); };
    a.makeRecord = [&](const RecordArgs& r) {
        FlowUtteranceRecord u;
        u.id = "r" + std::to_string(++seq);
        u.personaId = r.persona.id;
        u.personaName = r.persona.name;
        u.role = r.persona.role;
        u.text = r.text;
        u.turn = r.turn;
        if (r.act) u.act = r.act;
        return u;
    };
    a.commit = [&](const FlowUtteranceRecord& u) { out.records.push_back(u); };
    a.applyGraph = [](const FlowUtteranceRecord&, const DialogueAct&, const std::optional<std::string>&) {};
    out.end = run_ground_exchange(a);
    for (const auto& [id, s] : ex.ground_states())
        if (id == "pro-G1") out.state = s;
    out.turnCalls = turn_llm.calls.size();
    return out;
}

std::string acts(const Run& r) {
    std::string s;
    for (const auto& u : r.records) s += (u.act ? *u.act : (u.role == "facilitator" ? std::string("fac") : std::string("-"))) + ",";
    return s;
}

const char* kNoWarrant = R"({"act":"rebut","attack":"data","data":"課金しない人が 9 割","text":"9 割は課金してないよ"})";
const char* kGood =
    R"({"act":"rebut","attack":"data","kind":"induction","data":"課金しない人が 9 割","warrant":"売上は一部の人に偏っていて、必要とは言い切れない","text":"売上は一部の人だけだよ"})";

} // namespace

TEST_CASE("深掘り: 論拠の不足 (データ・論拠が無い / 形が崩れて読めない) だけを対象にし、突く先の欠けと前提ルール違反は対象外") {
    ParsedExchangeTurn degraded;
    degraded.act = "question";
    degraded.degraded = true;
    CHECK_EQ(missing_rebut_elements(degraded, std::nullopt).size(), std::size_t{2});
    const auto no_warrant = missing_rebut_elements(rebut(std::string("d"), std::nullopt, std::string("data")), gate(false, "incomplete"));
    CHECK_EQ(no_warrant.size(), std::size_t{1});
    CHECK(no_warrant[0].find("論拠") != std::string::npos);
    CHECK(missing_rebut_elements(rebut(std::string("d"), std::string("w"), std::nullopt), gate(false, "unanchored")).empty());
    CHECK(missing_rebut_elements(rebut(std::string("d"), std::string("w"), std::string("data")), gate(false, "straw_man")).empty());
    CHECK(missing_rebut_elements(rebut(std::string("d"), std::string("w"), std::string("data")), gate(true, "")).empty());
    ParsedExchangeTurn concede;
    concede.act = "concede";
    CHECK(missing_rebut_elements(concede, std::nullopt).empty());
}

TEST_CASE("深掘り: 進行役の文と言い直しの節は欠けている要素と 3 種類の論拠の形を示す") {
    const std::string req = render_drilldown_request("高瀬", {"反論の論拠"});
    CHECK(req.find("高瀬さん") != std::string::npos);
    CHECK(req.find("反論の論拠") != std::string::npos);
    const std::string block = drilldown_prompt_block({"反論の論拠"}, "前の応答");
    CHECK(block.find("演繹") != std::string::npos);
    CHECK(block.find("帰納") != std::string::npos);
    CHECK(block.find("アブダクション") != std::string::npos);
    CHECK(block.find("前の応答") != std::string::npos);
}

TEST_CASE("深掘り: 定立の論拠の不足 (形が崩れた・論拠なし・主張の複製) を見つけ、種類の欠けと LLM 障害は対象外") {
    GeneratedPosition p;
    p.claim = "主張";
    p.qualifier = "コア層";
    p.qualifierForce = "多くの場合";
    Ground g;
    g.id = "pro-G1";
    g.text = "データ";
    g.warrant = "論拠";
    g.kind = std::string("deduction");
    p.grounds = {g};
    CHECK(missing_position_elements(p, false).empty());
    GeneratedPosition no_warrant = p;
    no_warrant.grounds[0].warrant.reset();
    no_warrant.grounds[0].kind.reset();
    CHECK_EQ(missing_position_elements(no_warrant, false).size(), std::size_t{1});
    GeneratedPosition copied = p;
    copied.grounds[0].text = "主張";
    CHECK_EQ(missing_position_elements(copied, false).size(), std::size_t{1});
    GeneratedPosition degraded = p;
    degraded.degraded = true;
    CHECK_EQ(missing_position_elements(degraded, false).size(), std::size_t{2});
    CHECK(missing_position_elements(degraded, true).empty());
    CHECK(position_drilldown_feedback({"各根拠の論拠"}).find("各根拠の論拠") != std::string::npos);
}

TEST_CASE("深掘り: 論拠の無い反論は問いで残し、進行役が名指しして言い直させ、整えば反論として数える") {
    const Run r = run_one(Json::array({ok(kNoWarrant), ok(kGood), ok(R"({"act":"concede","text":"認めます"})")}), true);
    // 反対の最初の応答 (問い) → 進行役の深掘り → 言い直しの反論 → (賛成の応答) …
    CHECK(acts(r).rfind("question,fac,rebut,", 0) == 0);
    CHECK(r.records[1].text.find("反論の論拠") != std::string::npos);
    CHECK_EQ(r.end.drilled, 1);
    CHECK_EQ(r.end.drillRecovered, 1);
    // 言い直しのプロンプトに深掘りの節が載る (2 回目の呼び出し)。
    CHECK(r.turnCalls >= 2);
}

TEST_CASE("深掘り: 言い直しでも形が整わなければ従来どおり問いに格下げし、相手の論拠は成り立ったまま") {
    const Run r = run_one(Json::array({ok(kNoWarrant), ok(kNoWarrant)}), true);
    CHECK_EQ(r.end.drilled, 1);
    CHECK_EQ(r.end.drillRecovered, 0);
    CHECK_EQ(r.state, std::string("unchallenged"));
    CHECK(acts(r).rfind("question,fac,question,fac,", 0) == 0);
}

TEST_CASE("深掘り: 無効 (drillDown=false) なら言い直さない (従来の挙動)") {
    const Run r = run_one(Json::array({ok(kNoWarrant)}), false);
    CHECK_EQ(r.end.drilled, 0);
    CHECK_EQ(r.turnCalls, std::size_t{1});
    CHECK(acts(r).rfind("question,fac,", 0) == 0);
}

DI_TEST_MAIN()
