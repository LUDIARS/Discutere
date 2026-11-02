#pragma once
// 議論特化 persona seed (src/persona-engine/seeds/personas.ts)。
// 8 軸 (提案 / 反論 / 精緻化 / 観察 / 検証 / 統合 / 歴史 / 革新) + option 2 軸 (緊張緩和 / 沈黙)。
#include <vector>

#include "persona_engine/types.hpp"

namespace di::persona_engine::seeds {

const std::vector<PersonaSeed>& DISCUSSION_PERSONA_SEEDS();

} // namespace di::persona_engine::seeds
