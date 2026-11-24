#pragma once

#include <string>
#include <vector>

#include "flow/personas.hpp"

namespace di::flow {

/** プールから選んだ非進行役に、セッションで使用するテンプレート ID を割り当てる。 */
void assign_debater_templates(std::vector<FlowPersona>& cast, const std::string& session_id);

} // namespace di::flow
