// w4p_flow のテスト (4/4)。準備は w4p_flow_support.hpp。
#include "w4p_flow_support.hpp"

using namespace w4p_flow_support;

// ─── 境界 ───────────────────────────────────────────────────────

TEST_CASE("境界: split_for_post の上限ちょうど / +1 / 0 / 巨大な入力") {
    CHECK_EQ(foresight::split_for_post(std::string(1780, 'a')).size(), 1u);
    CHECK_EQ(foresight::split_for_post(std::string(1781, 'a')).size(), 2u);
    CHECK_EQ(foresight::split_for_post(std::string(1779, 'a') + "\nb").size(), 2u);  // 1779 + 1 + 1 = 1781 > 1780
    CHECK_EQ(foresight::split_for_post(std::string(1778, 'a') + "\nb").size(), 1u);
    CHECK_EQ(foresight::split_for_post(std::string(1779, 'a') + "\nbb").size(), 2u);
    CHECK_EQ(foresight::split_for_post("abc", 0).size(), 3u);  // 0 は 1 として扱う
    // 壊れた UTF-8 も落ちない
    (void)foresight::split_for_post(std::string("\xff\xfe\x80", 3), 1);
    // 巨大な入力 (8MB) を線形に分ける
    std::string huge;
    for (int i = 0; i < 80000; ++i) huge += std::string(99, 'x') + "\n";
    const auto chunks = foresight::split_for_post(huge);
    std::size_t total = 0;
    for (const auto& c : chunks) {
        CHECK(str::count_codepoints(c) <= foresight::POST_LIMIT);
        total += c.size();
    }
    CHECK(total + chunks.size() >= huge.size() - 1);
    CHECK_EQ(foresight::normalize_camp_turns(1e18), foresight::MAX_CAMP_TURNS);
    CHECK_EQ(foresight::normalize_camp_turns(foresight::MAX_CAMP_TURNS + 1), foresight::MAX_CAMP_TURNS);
    CHECK_EQ(foresight::camp_stage_at(-1), std::string("conditions"));
}

TEST_CASE("境界: parse_flow_kind / vote 表示への巨大な入力") {
    std::string big(1u << 20, 'x');
    CHECK(!parse_flow_kind(std::string_view(big)));
    CHECK_EQ(parse_flow_kind(std::string_view(big + "改善")).value_or(""), std::string("improvement"));
    VoteDisplay e;
    e.kind = "votes";
    for (int i = 0; i < 100000; ++i) e.tally.emplace_back("u" + std::to_string(i), i % 7);
    const auto lines = build_vote_ranking_lines(e, [](const std::string& id) { return id; });
    CHECK(lines.size() < 100000u);  // 0 票は出さない
    CHECK(str::starts_with(lines[0], "1. "));
    e.kind = "scores";
    e.tally = {{"a", std::nan("")}, {"b", 1.0}};
    CHECK_EQ(build_vote_ranking_lines(e, nullptr).size(), 2u);
}

TEST_CASE("境界: 壊れた・巨大な・失敗する LLM 応答でも dialectic は完走する (degrade)") {
    for (int variant = 0; variant < 3; ++variant) {
        w3l::TempFlowDb tmp;
        set_env(golden_run("dlx-basic")["env"]);
        std::string garbage = variant == 0 ? std::string(2u << 20, '{') : variant == 1 ? std::string("[1,2,{\"a\":") : std::string();
        w3l::FnLLM llm([&](const persona_engine::LLMInvokeArgs&) {
            if (variant == 2) return persona_engine::LLMResult::failure("down");
            return persona_engine::LLMResult::success(garbage);
        });
        FlowRunOptions o;
        o.sessionId = "dlx-garbage";
        o.rng = w3l::lcg(5);
        o.gamesDir = tmp.dir + "/games";
        o.log = [](const std::string&) {};
        o.warn = [](const std::string&) {};
        const auto r = dialectic::run_dialectic_flow(llm, "壊れたテーマ", {}, o);
        CHECK(r.rounds >= 1);
        CHECK(llm.count > 0);
        auto db = flow::db::get_flow_db();
        CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM flow_conclusion WHERE session_id = 'dlx-garbage'")->i64("n"), 1);
    }
    set_env(Json::object());
}

TEST_CASE("run_ground_exchange: 必須の引数が無ければ拒否 / ensure_debaters は昇格できなければ例外") {
    dialectic::GroundExchangeArgs a;
    CHECK_THROWS(dialectic::run_ground_exchange(a));
    FlowPersona f;
    f.id = "f";
    f.role = "facilitator";
    CHECK_THROWS(dialectic::ensure_debaters({f}, nullptr));
}

int main() { return di::test::run_all(); }
