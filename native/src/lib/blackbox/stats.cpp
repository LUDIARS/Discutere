#include "lib/blackbox/stats.hpp"

namespace di::lib::blackbox {

DomainStats domain_stats(const std::string& domain, const std::vector<DecisionRecord>& recent, const std::vector<Rule>& rules) {
    DomainStats s;
    s.domain = domain;
    s.window = static_cast<std::int64_t>(recent.size());
    for (const auto& d : recent) {
        if (d.source == "rule") ++s.ruleDecisions;
        if (d.source == "llm") ++s.llmDecisions;
        if (d.status == "pending_review" && !d.verdict) ++s.pendingReview;
    }
    for (const auto& r : rules) {
        if (r.state == "candidate") ++s.ruleStates.candidate;
        else if (r.state == "trial") ++s.ruleStates.trial;
        else if (r.state == "auto") ++s.ruleStates.autoCount;
        else if (r.state == "retired") ++s.ruleStates.retired;
    }
    s.ruleCoverage = recent.empty() ? 0 : static_cast<double>(s.ruleDecisions) / static_cast<double>(recent.size());
    return s;
}

Json to_json(const DomainStats& s) {
    return Json{{"domain", s.domain},
                {"window", s.window},
                {"ruleDecisions", s.ruleDecisions},
                {"llmDecisions", s.llmDecisions},
                {"ruleCoverage", s.ruleCoverage},
                {"pendingReview", s.pendingReview},
                {"ruleStates",
                 Json{{"candidate", s.ruleStates.candidate},
                      {"trial", s.ruleStates.trial},
                      {"auto", s.ruleStates.autoCount},
                      {"retired", s.ruleStates.retired}}}};
}

} // namespace di::lib::blackbox
