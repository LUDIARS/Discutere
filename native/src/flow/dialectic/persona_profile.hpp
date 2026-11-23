#pragma once
// 発言者の遊び手としての型を dialectic の prompt に載せる 1 行 (spec/plan/2026-11-23-persona-game-archetype-context.md A1)。
//
// 出所は 2 つ (どちらも無ければ空 = 従来とバイト等価):
//   - playerProfile: personaIds で指定したプール人物の descriptor (嗜好軸・苦手・メカニクス反応・年代/課金)
//   - possession:    議題に近いプール人物を憑依した場合の descriptor
// 代理議論の人物は載せない (本人の発言に縛るため。型からの推測を本人由来として混ぜない)。
#include <string>

#include "flow/personas.hpp"

namespace di::flow::dialectic {

/** 型の行 (末尾改行なし)。載せるものが無ければ空。 */
std::string persona_profile_line(const FlowPersona& persona);

} // namespace di::flow::dialectic
