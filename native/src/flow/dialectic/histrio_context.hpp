#pragma once

#include "flow/dialectic/argument_pattern_catalog.hpp"
#include "flow/effect_predict.hpp"
#include "flow/personas.hpp"
#include "integrations/histrio_client.hpp"

namespace di::flow::dialectic {

/** 通常の論述パターン割当後に、プール人物の脚本をセッション内で確定する。 */
void apply_histrio_context(std::vector<FlowPersona>& cast, const integrations::HistrioClient& client,
                           const ArgumentPatternCatalog& local, const WarnFn& warn);

} // namespace di::flow::dialectic
