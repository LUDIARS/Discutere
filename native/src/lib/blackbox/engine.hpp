#pragma once
// 成長型ブラックボックスの中核アルゴリズム (engine.ts)。
//
// decide():         live ルール (trial/auto) 優先 → 無ければ LLM フォールバック。どちらの経路でも
//                   candidate ルールを影評価し、教師 (auto ルール / LLM の出力) との一致・衝突を蓄積する。
// record_verdict(): 人間の OK/NG。trial ルールの昇格 / 撤回に加え、LLM 判断への NG は
//                   その判断を教師にした影評価を反転する (誤った教師で育てない)。
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "lib/blackbox/lifecycle.hpp"
#include "lib/blackbox/types.hpp"

namespace di::lib::blackbox {

struct EngineOptions {
    std::optional<std::int64_t> autoPromote;
    std::optional<std::int64_t> autoRetract;
    std::optional<std::int64_t> shadowPromote;
    std::optional<std::int64_t> shadowConflictLimit;
    /** LLM 由来の判断も pending_review としてレビューキューに載せるか (既定 true)。 */
    std::optional<bool> reviewLlmDecisions;
    /** 時刻源 (既定は現在時刻の ISO 文字列)。 */
    std::function<std::string()> now;
};

struct DecideResult {
    Decision decision;
    std::int64_t decisionId = 0;
};

struct VerdictResult {
    bool ok = false;
    std::optional<Rule> ruleUpdated;
};

class BlackBoxEngine {
public:
    BlackBoxEngine(std::shared_ptr<RuleStore> rules, std::shared_ptr<DecisionLedger> ledger, const EngineOptions& opts = {});

    /** domain の判断を下す。結果は ledger に記録され decisionId が付与される。 */
    DecideResult decide(const std::string& domain, const Json& input, const FeatureMap& features,
                        const LlmFallback& llm_fallback);
    /** 人間の OK/NG ("ok" / "ng") を記録する。 */
    VerdictResult record_verdict(std::int64_t decision_id, const std::string& verdict);
    /** 手動 / 採掘でルールを追加する。manual は trial から始まる。 */
    Rule add_rule(const RuleDraft& draft);
    /** ルールの状態を手動で変更する。 */
    std::optional<Rule> set_rule_state(const std::string& id, const RuleState& state);
    std::vector<Rule> list_rules(const std::string& domain);

private:
    std::optional<Rule> first_live_match(const std::vector<Rule>& rules, const FeatureMap& features) const;
    std::optional<std::string> propose(const std::string& domain, const RuleProposal& proposal);
    std::vector<ShadowEval> run_shadow(const std::vector<Rule>& rules, const FeatureMap& features, const Json& teacher_output,
                                       const std::optional<std::string>& exclude_id = std::nullopt);

    std::shared_ptr<RuleStore> rules_;
    std::shared_ptr<DecisionLedger> ledger_;
    Thresholds t_;
    bool review_llm_;
    std::function<std::string()> now_;
};

} // namespace di::lib::blackbox
