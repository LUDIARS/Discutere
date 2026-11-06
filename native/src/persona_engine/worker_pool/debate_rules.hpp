#pragma once
// 8 固定キャスト用の debate ルール seed (src/persona-engine/worker-pool/debate-rules.ts)。
// target を worker id に合わせる (engine は personaId = rule.target で発火する)。
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "persona_engine/types.hpp"

namespace di::persona_engine::worker_pool {

const std::vector<RuleSeed>& DEBATE_RULE_SEEDS();
/** ルール id → 既定 instructions (runtime override の比較・デフォルト用)。 */
const std::map<std::string, std::string>& RULE_INSTRUCTION_DEFAULTS();

/**
 * 各ルールの instructions に runtime override を適用した新しい seed 配列を返す (pure)。
 * override が無い (nullopt / 空白のみ) ルールは元の instructions を保つ。
 */
std::vector<RuleSeed> apply_rule_instruction_overrides(
    const std::vector<RuleSeed>& seeds, const std::function<std::optional<std::string>(const std::string&)>& get_instruction);

} // namespace di::persona_engine::worker_pool
