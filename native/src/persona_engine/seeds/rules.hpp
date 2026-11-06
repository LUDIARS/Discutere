#pragma once
// 議論駆動 rule seed (src/persona-engine/seeds/rules.ts, Phase 0)。
// 「行動を返さない (skip)」も valid な選択肢 (過剰発話を防ぐ)。
#include <set>
#include <string>
#include <vector>

#include "persona_engine/types.hpp"

namespace di::persona_engine::seeds {

const std::vector<RuleSeed>& DISCUSSION_RULE_SEEDS();
/** seed rule は AI による削除から保護する (= remove_rule で拒否) */
const std::set<std::string>& SEED_RULE_IDS();

} // namespace di::persona_engine::seeds
