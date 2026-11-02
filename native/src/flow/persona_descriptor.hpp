#pragma once
// src/flow/persona-descriptor.ts の移植: 憑依 descriptor (実データ由来の人物像を prompt 用の 1 文にする)。純関数。
#include <string>

#include "flow/persona_pool.hpp"

namespace di::flow {

/** 嗜好軸 id の日本語名 (未知の軸は id のまま)。 */
std::string persona_axis_label(const std::string& axis);

std::string build_persona_descriptor(const PoolPersona& persona);

} // namespace di::flow
