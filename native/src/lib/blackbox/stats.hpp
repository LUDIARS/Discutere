#pragma once
// 卒業メトリクス (stats.ts)。純粋関数のみ。
#include <string>
#include <vector>

#include "lib/blackbox/types.hpp"

namespace di::lib::blackbox {

struct RuleStateCounts {
    std::int64_t candidate = 0;
    std::int64_t trial = 0;
    std::int64_t autoCount = 0;  // JSON では "auto"
    std::int64_t retired = 0;
};

struct DomainStats {
    std::string domain;
    /** 集計対象にした直近判断数。 */
    std::int64_t window = 0;
    std::int64_t ruleDecisions = 0;
    std::int64_t llmDecisions = 0;
    /** ルール由来の割合 0..1。判断が無ければ 0。 */
    double ruleCoverage = 0;
    /** verdict 待ちの判断数。 */
    std::int64_t pendingReview = 0;
    RuleStateCounts ruleStates;
};

DomainStats domain_stats(const std::string& domain, const std::vector<DecisionRecord>& recent, const std::vector<Rule>& rules);
Json to_json(const DomainStats& s);

} // namespace di::lib::blackbox
