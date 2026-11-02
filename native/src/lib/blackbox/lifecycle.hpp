#pragma once
// ルールのライフサイクル遷移 (candidate → trial → auto / retired) (lifecycle.ts)。純粋関数のみ。
#include <optional>

#include "lib/blackbox/types.hpp"

namespace di::lib::blackbox {

struct Thresholds {
    /** trial ルールへの人間 OK がこの数に達すると auto (卒業)。 */
    std::int64_t autoPromote = 3;
    /** 人間 NG がこの数に達すると retired (撤回)。 */
    std::int64_t autoRetract = 3;
    /** candidate の影一致がこの数に達する (かつ衝突 0) と trial へ昇格。 */
    std::int64_t shadowPromote = 3;
    /** candidate の影衝突がこの数に達すると retired。 */
    std::int64_t shadowConflictLimit = 2;
};

inline constexpr Thresholds DEFAULT_THRESHOLDS{};

struct ShadowDelta {
    std::int64_t agreements = 0;
    std::int64_t conflicts = 0;
};

/** 影評価カウンタの増減から candidate の次状態を含む patch を作る。candidate 以外は nullopt。 */
std::optional<RulePatch> shadow_patch(const Rule& rule, const ShadowDelta& delta, const Thresholds& t);
/** 人間の OK/NG によるルールの patch (verdict は "ok" / "ng")。 */
RulePatch verdict_patch(const Rule& rule, const std::string& verdict, const Thresholds& t);
/** 発火対象 (live) のルールか。 */
bool is_live(const RuleState& state);

} // namespace di::lib::blackbox
