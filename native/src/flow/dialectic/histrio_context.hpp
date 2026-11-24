#pragma once

#include "flow/dialectic/argument_pattern_catalog.hpp"
#include "flow/effect_predict.hpp"
#include "flow/personas.hpp"
#include "integrations/histrio_client.hpp"

namespace di::flow::dialectic {

/** 割り当て済みの脚本を、取得できたテンプレートまたは Steam 代理の脚本で更新する。 */
void apply_histrio_context(std::vector<FlowPersona>& cast, const integrations::HistrioClient& client,
                           const ArgumentPatternCatalog& local, const WarnFn& warn);

} // namespace di::flow::dialectic
