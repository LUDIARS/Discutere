// 論述パターン (spec/plan/2026-11-23-argument-patterns.md) の受入。
#include "flow/w2g_support.hpp"

#include <set>

#include "base/fsutil.hpp"
#include "flow/db/connection.hpp"
#include "flow/dialectic/argument_pattern.hpp"
#include "flow/dialectic/exchange_prompt.hpp"
#include "flow/dialectic/position.hpp"
#include "flow/persona_setup.hpp"

using namespace di;
using namespace di::flow;
using namespace di::flow::dialectic;

namespace {

FlowPersona person(const std::string& id, const std::string& name, const std::string& role, const std::string& stance) {
    FlowPersona p;
    p.id = id;
    p.name = name;
    p.role = role;
    p.stance = stance;
    p.speechStyle = "普通";
    return p;
}

std::vector<FlowPersona> sample_cast() {
    return {person("f", "進行", "facilitator", "neutral"), person("a", "宮本", "debater", "pro"), person("b", "高瀬", "debater", "con"),
            person("c", "小野", "debater", "pro"),        person("d", "森", "debater", "con"),   person("e", "原", "debater", "pro")};
}

PositionRecord own_position(const std::string& stance) {
    PositionRecord p;
    p.id = "P-" + stance;
    p.personaId = stance == "pro" ? "a" : "b";
    p.stance = stance;
    p.claim = "主張";
    Ground g;
    g.id = stance + "-G1";
    g.text = "データ";
    g.warrant = std::string("論拠");
    p.grounds.push_back(g);
    return p;
}

} // namespace

TEST_CASE("論述パターン: 組み込み版のカタログは data/argument-patterns.json と同じ") {
    const std::string path = di::test::fixtures_dir() + "/../../../data/argument-patterns.json";
    auto text = fs::read_file(path);
    CHECK(text.has_value());
    CHECK(parse_json_or_throw(*text) == parse_json_or_throw(embedded_argument_pattern_catalog_json()));
    const auto c = parse_argument_pattern_catalog(parse_json_or_throw(*text));
    CHECK_EQ(c.scriptPatterns.size(), std::size_t(6));
    CHECK_EQ(c.reasoningStyles.size(), std::size_t(3));
    CHECK(find_script_pattern(c, "never") != nullptr);
    CHECK_EQ(find_script_pattern(c, "いつも")->id, std::string("always"));
    CHECK_EQ(find_reasoning_style(c, "帰納")->id, std::string("induction"));
    CHECK(find_script_pattern(c, "どれでもない") == nullptr);
}

TEST_CASE("論述パターン: 割り当ては決定的で、同じ陣営に同じパターンを並べない。進行役と代理には付けない") {
    const auto& cat = argument_pattern_catalog();
    auto cast1 = sample_cast();
    FlowPersona proxy = person("x", "代理", "debater", "con");
    proxy.proxy = ProxyPersonaContext{};
    cast1.push_back(proxy);
    auto cast2 = cast1;
    assign_argument_patterns(cast1, "ガチャは必要か", {}, cat);
    assign_argument_patterns(cast2, "ガチャは必要か", {}, cat);
    for (std::size_t i = 0; i < cast1.size(); ++i) {
        CHECK(cast1[i].scriptPattern == cast2[i].scriptPattern);
        CHECK(cast1[i].reasoningStyle == cast2[i].reasoningStyle);
    }
    CHECK(!cast1[0].scriptPattern);  // 進行役
    CHECK(!cast1.back().scriptPattern);  // 代理
    std::set<std::string> pro_p, pro_r;
    for (const auto& p : cast1)
        if (p.stance == "pro") {
            CHECK(p.scriptPattern.has_value());
            pro_p.insert(*p.scriptPattern);
            pro_r.insert(*p.reasoningStyle);
        }
    CHECK_EQ(pro_p.size(), std::size_t(3));  // 賛成 3 人はパターンが重ならない
    CHECK_EQ(pro_r.size(), std::size_t(3));  // 論じ方の好みも 3 種類に分かれる
}

TEST_CASE("論述パターン: ペーパーの「# 論じ方の癖」で人が上書きできる") {
    const auto& cat = argument_pattern_catalog();
    const std::string md = "# 議題\nガチャ\n\n# 論じ方の癖\n- 宮本: 決して / 帰納\n- 高瀬：open_ended\n- 誰か: 不明な値\n\n# 論点\n- 宮本: always\n";
    const auto o = parse_argument_pattern_overrides(md, cat);
    CHECK_EQ(o.size(), std::size_t(2));
    CHECK_EQ(*o.at("宮本").scriptPattern, std::string("never"));
    CHECK_EQ(*o.at("宮本").reasoningStyle, std::string("induction"));
    CHECK_EQ(*o.at("高瀬").scriptPattern, std::string("open_ended"));
    CHECK(!o.at("高瀬").reasoningStyle);
    auto cast = sample_cast();
    assign_argument_patterns(cast, "ガチャは必要か", o, cat);
    CHECK_EQ(*cast[1].scriptPattern, std::string("never"));
    CHECK_EQ(*cast[1].reasoningStyle, std::string("induction"));
    CHECK_EQ(*cast[2].scriptPattern, std::string("open_ended"));
    for (const auto& p : cast)  // 上書きした never は賛成の他の人に重ねない
        if (p.stance == "pro" && p.id != "a") CHECK(*p.scriptPattern != std::string("never"));
}

TEST_CASE("論述パターン: 定立・応酬・押されたときで行が変わり、名前は名乗らせない") {
    const auto& cat = argument_pattern_catalog();
    FlowPersona p = person("a", "宮本", "debater", "pro");
    p.scriptPattern = "until";
    p.reasoningStyle = "abduction";
    const auto* sp = find_script_pattern(cat, "until");
    const auto pos = argument_pattern_lines(p, ArgumentPhase::position, false, cat);
    const auto ex = argument_pattern_lines(p, ArgumentPhase::exchange, false, cat);
    const auto st = argument_pattern_lines(p, ArgumentPhase::exchange, true, cat);
    CHECK_EQ(pos.size(), std::size_t(2));
    CHECK(pos[0].find(sp->position) != std::string::npos);
    CHECK(ex[0].find(sp->exchange) != std::string::npos);
    CHECK(st[0].find(sp->stress) != std::string::npos);
    CHECK(pos[0].find("名乗らない") != std::string::npos);
    CHECK(pos[0].find("until") == std::string::npos);
    CHECK(pos[1].find("アブダクション") != std::string::npos);
    FlowPersona none = person("b", "高瀬", "debater", "con");
    CHECK(argument_pattern_lines(none, ArgumentPhase::exchange, true, cat).empty());
}

TEST_CASE("論述パターン: 定立と応酬の prompt に癖が載り、押されていればストレス側になる") {
    const auto& cat = argument_pattern_catalog();
    const auto* sp = find_script_pattern(cat, "never");
    FlowPersona p = person("a", "宮本", "debater", "pro");
    p.scriptPattern = "never";
    p.reasoningStyle = "induction";

    BuildPositionPromptArgs bp{p, "pro", {}, std::nullopt, std::nullopt};
    bp.issue.ordinal = 1;
    bp.issue.title = "ガチャは必要か";
    CHECK(build_position_prompt(bp).find(sp->position) != std::string::npos);

    ExchangePromptArgs pa;
    pa.persona = p;
    pa.stance = "pro";
    pa.issue.ordinal = 1;
    pa.issue.title = "ガチャは必要か";
    pa.positions = {own_position("pro"), own_position("con")};
    ArgumentNode root;
    root.id = "N1";
    root.side = "con";
    root.personaId = "b";
    root.groundId = std::string("con-G1");
    root.data = "データ";
    root.text = "say";
    pa.path = {root};
    const std::string calm = build_exchange_prompt(pa);
    CHECK(calm.find(sp->exchange) != std::string::npos);
    CHECK(calm.find(sp->stress) == std::string::npos);
    pa.pressed = true;
    const std::string pressed = build_exchange_prompt(pa);
    CHECK(pressed.find(sp->stress) != std::string::npos);
    CHECK(pressed.find(sp->exchange) == std::string::npos);
}

TEST_CASE("論述パターン: 割り当てとカタログの版を flow_session_persona に残す") {
    w2g::TempFlowDb tmp;
    auto cast = sample_cast();
    persist_session_personas("S1", cast);
    assign_argument_patterns(cast, "ガチャは必要か", {}, argument_pattern_catalog());
    record_session_argument_patterns("S1", cast, argument_pattern_catalog().version);
    auto db = db::get_flow_db();
    const auto rows = db->all("SELECT id, script_pattern, reasoning_style, pattern_catalog_version FROM flow_session_persona WHERE session_id = 'S1' ORDER BY id");
    CHECK_EQ(rows.size(), cast.size());
    for (const auto& r : rows) {
        if (r.str("id") == "f") {
            CHECK(r.value(1).to_json().is_null());
            continue;
        }
        CHECK(!r.str("script_pattern").empty());
        CHECK(!r.str("reasoning_style").empty());
        CHECK_EQ(r.str("pattern_catalog_version"), argument_pattern_catalog().version);
    }
}

DI_TEST_MAIN()
