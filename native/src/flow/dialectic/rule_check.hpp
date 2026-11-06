#pragma once
// src/flow/dialectic/rule-check.ts の移植: 前提ルール違反の検査 (dialectic.md §2.5)。
//   1. check_rebut_anchor (コードのみ・決定的): 反論自身のデータと論拠・突く要素・対象の根拠 id。
//   2. judge_rule_violation (判定 LLM): 範囲外・すり替え・中身に触れない却下・論点ずらし・立場の取り違え。
// LLM 障害・ラベル不明は ok に倒す (議論を止めない、warn 明示)。
#include <optional>
#include <string>

#include "flow/dialectic/premise_rules.hpp"
#include "flow/dialectic/store.hpp"
#include "flow/effect_predict.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::dialectic {

/** incomplete / unanchored / out_of_scope / straw_man / dismissal / topic_shift / self_contradiction /
 *  unsupported_attribution / quote_mismatch */
using RuleFoul = std::string;

/** RULE_FOUL_LABEL[foul]。未知は空。 */
std::string rule_foul_label(const RuleFoul& foul);

struct AnchorResult {
    bool ok = false;
    /** ok のとき: 突かれた根拠 (qualifier なら nullopt)。 */
    std::optional<Ground> ground;
    /** !ok のとき: "incomplete" | "unanchored" */
    std::string foul;
};

struct RebutClaim {
    std::optional<AttackPoint> attack;
    std::optional<std::string> groundId;
    std::optional<std::string> data;
    std::optional<std::string> warrant;
};

AnchorResult check_rebut_anchor(const RebutClaim& claim, const PositionRecord& position);

struct RuleJudgeInput {
    IssueRecord issue;
    PositionRecord position;
    std::optional<PositionRecord> ownPosition;
    std::optional<Ground> ground;
    AttackPoint attack;
    std::string rebutText;
    std::string rebutData;
    std::string rebutWarrant;
};

std::string build_rule_judge_prompt(const RuleJudgeInput& args);
/** 最初に現れた既知ラベル ("ok" を含む)。無ければ nullopt。 */
std::optional<std::string> parse_rule_verdict(const std::string& text);
/** 判定 LLM で前提ルール違反を検査する。"ok" か RuleFoul。 */
std::string judge_rule_violation(persona_engine::LLMClient& llm, const RuleJudgeInput& input,
                                 const std::optional<std::string>& model = std::nullopt, const WarnFn& warn = {});

struct RebutGateResult {
    bool accepted = false;
    std::optional<Ground> ground;
    RuleFoul foul;
};

struct GateRebutArgs {
    RebutClaim claim;
    IssueRecord issue;
    PositionRecord position;
    std::optional<PositionRecord> ownPosition;
    std::string rebutText;
    bool judge = true;
    std::optional<std::string> model;
    WarnFn warn;
};
RebutGateResult gate_rebut(persona_engine::LLMClient& llm, const GateRebutArgs& args);

std::string build_concession_prompt(const std::string& target_text, const std::string& reply_text);
/** 「認める」と申告した返答が本当に認めているか ("concede" | "rebut")。障害・不明は concede。 */
std::string judge_concession(persona_engine::LLMClient& llm, const std::string& target_text, const std::string& reply_text,
                             const std::optional<std::string>& model = std::nullopt, const WarnFn& warn = {});

std::string render_foul_notice(const std::string& persona_name, const RuleFoul& foul);

} // namespace di::flow::dialectic
