#pragma once
// src/flow/persona-cast.ts の移植: 通常議論のキャスト = 生成 facilitator + 指定されたプールペルソナ + 生成ペルソナで補完。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "flow/personas.hpp"

namespace di::flow {

struct BuildFlowCastArgs {
    int count = 0;
    std::string defaultModel;
    bool isLocal = false;
    Rng rng;
    std::vector<std::string> personaIds;
    /** モデル編成 (config flow.roster)。生成ペルソナのモデル割当に使う。 */
    std::optional<FlowModelRoster> roster;
    std::function<void(const std::string&)> warn;
};

std::vector<FlowPersona> build_flow_cast(const BuildFlowCastArgs& args);

} // namespace di::flow
