// flow/foresight の prompts / markdown / state (tests/flow/foresight.test.ts の該当部分 + golden fixtures/w1f/foresight.json)。
#include "check.hpp"
#include "w1f_support.hpp"

#include "base/strings.hpp"
#include "flow/foresight/markdown.hpp"
#include "flow/foresight/prompts.hpp"
#include "flow/foresight/state.hpp"
#include "flow/user_voices/reference_policy.hpp"

using namespace di;
using namespace di::flow::foresight;
using namespace di::test::w1f;

namespace {

ForesightContext ctx() {
    return {"# 議題\nガチャ\n\n# 議論の経過\n止揚: 段階導入", {{"高すぎる", "steam"}}};
}

std::vector<ForesightLine> two_lines() { return {{"A", "reform", "やるべき"}, {"B", "conservative", "やめるべき"}}; }

void check_pair(const PromptPair& got, const Json& want, const std::string& label) {
    if (got.system != want["system"].get<std::string>()) throw test::Failure(label + " system differs:\n" + got.system);
    if (got.prompt != want["prompt"].get<std::string>()) throw test::Failure(label + " prompt differs:\n" + got.prompt);
}

} // namespace

TEST_CASE("context blocks match Node (cut at 6000 UTF-16 units, 8 voices)") {
    const Json fx = load_fixture("foresight.json");
    std::vector<ForesightVoice> many;
    for (int i = 0; i < 10; ++i) many.push_back({"声" + std::to_string(i), i % 2 ? "youtube" : "steam"});
    const std::string long_paper = str::repeat("あ", 5998) + "😀" + str::repeat("い", 10);
    const std::vector<std::optional<ForesightContext>> inputs{
        std::nullopt,
        ForesightContext{"", {}},
        ForesightContext{"   ", many},
        ForesightContext{std::string(6000, 'x'), {}},
        ForesightContext{std::string(6001, 'x'), {}},
        ForesightContext{long_paper, {}},
        ctx(),
    };
    for (std::size_t i = 0; i < inputs.size(); ++i) CHECK_EQ(context_block(inputs[i]), fx["contextBlocks"][i].get<std::string>());
}

TEST_CASE("proposal / camp / forecast prompts match Node byte for byte") {
    const Json fx = load_fixture("foresight.json");
    check_pair(build_proposal_prompt("ガチャ課金の未来", "ガチャは続く", ctx()), fx["proposals"][0], "proposal0");
    check_pair(build_proposal_prompt("t", ""), fx["proposals"][1], "proposal1");
    check_pair(build_proposal_prompt("t", "  結論  ", ForesightContext{"", {}}), fx["proposals"][2], "proposal2");

    CampTurnArgs a;
    a.theme = "t";
    a.proposal = "P";
    a.personaName = "A";
    a.camp = "reform";
    a.ownStatements = {"前に言ったこと", "  ", " 二つ目 "};
    check_pair(build_camp_turn_prompt(a), fx["camps"][0], "camp0");
    CampTurnArgs b;
    b.theme = "t";
    b.proposal = "P";
    b.personaName = "B";
    b.camp = "conservative";
    b.lines = two_lines();
    b.stage = "verification";
    check_pair(build_camp_turn_prompt(b), fx["camps"][1], "camp1");
    b.stage = "refutation";
    check_pair(build_camp_turn_prompt(b), fx["camps"][2], "camp2");

    ForecastArgs f;
    f.theme = "t";
    f.conclusion = "c";
    f.proposal = "P";
    f.context = ctx();
    check_pair(build_forecast_prompt(f), fx["forecasts"][0], "forecast0");
    ForecastArgs g;
    g.theme = "t";
    g.proposal = "P";
    g.lines = two_lines();
    check_pair(build_forecast_prompt(g), fx["forecasts"][1], "forecast1");

    CHECK_EQ(camp_stance_line("reform"), fx["stances"][0].get<std::string>());
    CHECK_EQ(camp_stance_line("conservative"), fx["stances"][1].get<std::string>());
    CHECK_EQ(camp_stage_line("conditions"), fx["stages"][0].get<std::string>());
    CHECK_EQ(camp_stage_line("refutation"), fx["stages"][1].get<std::string>());
    CHECK_EQ(camp_stage_line("verification"), fx["stages"][2].get<std::string>());
    CHECK_EQ(std::string(flow::user_voices::VOICE_REFERENCE_POLICY), fx["VOICE_REFERENCE_POLICY"].get<std::string>());
}

TEST_CASE("prompt content checks (foresight.test.ts)") {
    const auto proposal = build_proposal_prompt("ガチャ課金の未来", "ガチャは続く", ctx());
    CHECK(str::contains(proposal.system, "ローエンド型") && str::contains(proposal.system, "非消費者"));
    CHECK(str::contains(proposal.system, "キャズム (Moore)") && str::contains(proposal.system, "Rogers とは別の理論"));
    CHECK(!str::contains(proposal.system, "何を過剰に供給しているか"));
    CHECK(str::contains(proposal.prompt, "「高すぎる」(出所: steam)") && str::contains(proposal.prompt, "関連は未確認"));
    CHECK(str::contains(build_proposal_prompt("t", "").prompt, "(結論なし)"));
    CHECK_EQ(context_block(std::nullopt), "");
    CHECK(str::contains(context_block(ForesightContext{std::string(7000, 'x'), {}}), "(以下略)"));
}

TEST_CASE("strip / append foresight section matches Node (idempotent, legacy heading, CR lines)") {
    const Json fx = load_fixture("foresight.json");
    for (const auto& s : fx["strips"]) CHECK_EQ(strip_foresight(s["input"].get<std::string>()), s["output"].get<std::string>());
    for (const auto& a : fx["appends"]) {
        ForesightSection section{a["section"]["proposal"].get<std::string>(), a["section"]["forecast"].get<std::string>()};
        CHECK_EQ(append_foresight_markdown(a["body"].get<std::string>(), section), a["output"].get<std::string>());
    }
    const auto once = append_foresight_markdown("# 議題\nX\n\n# 結論\nY", {"P1", "F1"});
    const auto twice = append_foresight_markdown(once, {"P2", "F2"});
    CHECK_EQ(strip_foresight(twice), "# 議題\nX\n\n# 結論\nY");
    // 巨大な本文でも線形に処理できる
    const std::string huge = str::repeat("# 未来予測の補足ではない見出し\n", 20000) + "# 未来予測\n末尾";
    CHECK(str::ends_with(strip_foresight(huge), "見出し"));
}

TEST_CASE("foresight status (state.ts)") {
    reset_foresight_status_for_test();
    CHECK(!get_foresight_status("s").has_value());
    set_foresight_status("s", "running");
    CHECK(is_foresight_active("s"));
    CHECK_EQ(json_stringify(to_json(*get_foresight_status("s"))), R"({"state":"running"})");
    set_foresight_status("s", "skipped", "理由");
    CHECK(get_foresight_status("s") == (ForesightStatus{"skipped", std::string("理由")}));
    CHECK(!is_foresight_active("s"));
    set_foresight_status("p", "pending");
    CHECK(is_foresight_active("p"));
    reset_foresight_status_for_test();
    CHECK(!get_foresight_status("p").has_value());
}

DI_TEST_MAIN()
