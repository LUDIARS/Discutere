#pragma once
// 仮説のライフサイクル遷移を適用する (src/core/hypothesis/lifecycle-handler.ts)。
// 状態遷移を検査し (state_machine)、遷移先に対応するイベントを EventLog に積む。
#include <optional>
#include <string>

#include "core/hypothesis/state_machine.hpp"
#include "core/projection/types.hpp"

namespace di::core::hypothesis {

struct LifecycleExtra {
    std::string workspaceId;
    std::optional<std::string> sessionId;
};

/** { ok: true, to } | { ok: false, error }。仮説が無ければ error = "hypothesis not found"。 */
TransitionResult apply_lifecycle(projection::ReturnTypeCreateCore& core, const std::string& hypothesis_id, std::string_view action,
                                 const LifecycleExtra& extra = {});

} // namespace di::core::hypothesis
