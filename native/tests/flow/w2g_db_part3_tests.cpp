// w2g_db のテスト (3/3)。準備は w2g_db_support.hpp。
#include "w2g_db_support.hpp"

using namespace w2g_db_support;

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
