#include "lib/blackbox/lifecycle.hpp"

#include <algorithm>

namespace di::lib::blackbox {

namespace {
RuleState candidate_state_for(std::int64_t agreements, std::int64_t conflicts, const Thresholds& t) {
    if (conflicts >= t.shadowConflictLimit) return "retired";
    if (conflicts == 0 && agreements >= t.shadowPromote) return "trial";
    return "candidate";
}
} // namespace

std::optional<RulePatch> shadow_patch(const Rule& rule, const ShadowDelta& delta, const Thresholds& t) {
    if (rule.state != "candidate") return std::nullopt;
    const std::int64_t agreements = std::max<std::int64_t>(0, rule.shadowAgreements + delta.agreements);
    const std::int64_t conflicts = std::max<std::int64_t>(0, rule.shadowConflicts + delta.conflicts);
    RulePatch p;
    p.shadowAgreements = agreements;
    p.shadowConflicts = conflicts;
    p.state = candidate_state_for(agreements, conflicts, t);
    return p;
}

RulePatch verdict_patch(const Rule& rule, const std::string& verdict, const Thresholds& t) {
    RulePatch p;
    if (verdict == "ok") {
        const std::int64_t approvals = rule.approvals + 1;
        const bool graduated = rule.state == "trial" && approvals >= t.autoPromote;
        p.approvals = approvals;
        p.state = graduated ? RuleState("auto") : rule.state;
        return p;
    }
    const std::int64_t rejections = rule.rejections + 1;
    p.rejections = rejections;
    p.state = rejections >= t.autoRetract ? RuleState("retired") : rule.state;
    return p;
}

bool is_live(const RuleState& state) { return state == "trial" || state == "auto"; }

} // namespace di::lib::blackbox
