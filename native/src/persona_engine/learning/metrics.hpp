#pragma once
// Persona / Rule 指標 (src/persona-engine/learning/metrics.ts, PR-F / MISSING #3-2 + #3-3)。
// rule_log + persona.learned_notes から「採用率」を算出。hypothesis status の解決関数は注入。
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "persona_engine/db/personas_repo.hpp"
#include "persona_engine/db/rules_repo.hpp"

namespace di::persona_engine::learning {

struct PersonaMetric {
    std::string id;
    std::string name;
    std::string display_name;
    /** 提案 hypothesis 総数 */
    std::int64_t proposed = 0;
    /** integrated になった数 */
    std::int64_t integrated = 0;
    /** rejected になった数 */
    std::int64_t rejected = 0;
    /** integrated 率 (proposed > 0 のとき 0-1、0 件なら null) */
    std::optional<double> acceptanceRate;
};

struct RuleMetric {
    std::string id;
    std::int64_t fires = 0;
    std::int64_t skips = 0;
    std::int64_t errors = 0;
};

/** "integrated" | "rejected" | "pending" | "unknown" */
using HypothesisStatusResolver = std::function<std::string(const std::string& hypothesis_id)>;

std::vector<PersonaMetric> persona_metrics(db::PersonasRepo& personas, db::RulesRepo& rules,
                                           const HypothesisStatusResolver& resolve_status, std::int64_t scan_limit = 1000);
std::vector<RuleMetric> rule_metrics(db::RulesRepo& rules, std::int64_t scan_limit = 1000);

Json to_json(const PersonaMetric& m);
Json to_json(const RuleMetric& m);

} // namespace di::persona_engine::learning
