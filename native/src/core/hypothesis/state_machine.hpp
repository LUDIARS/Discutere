#pragma once
// 仮説の状態遷移 (src/core/hypothesis/state-machine.ts)。
#include <optional>
#include <string>
#include <string_view>

namespace di::core::hypothesis {

// HypothesisState: "draft" | "proposed" | "under_theory_validation" | "validated_by_theory" |
//   "under_emotion_validation" | "validated_by_emotion" | "integrated" | "rejected"
// HypothesisAction: "submit" | "validate_theory" | "validate_emotion" | "session_end" | "integrate" | "reject"

struct TransitionResult {
    bool ok = false;
    std::string to;     // ok のとき
    std::string error;  // !ok のとき
};

TransitionResult can_transition(std::string_view from, std::string_view action);

} // namespace di::core::hypothesis
