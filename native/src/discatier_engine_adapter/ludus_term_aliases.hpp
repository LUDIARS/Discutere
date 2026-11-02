#pragma once
// Ludus game-lexicon 由来の用語別名グループ (src/discatier-engine-adapter/ludus-term-aliases.ts の写し)。
// TS 側は scripts/build-ludus-term-aliases.ts の自動生成物。中身が TS と同じことは
// native/tests/discatier_engine_adapter/game_aliases_tests.cpp が golden で確かめる。
#include <string>
#include <vector>

namespace di::discatier_engine_adapter {

const std::vector<std::vector<std::string>>& ludus_term_alias_groups();

} // namespace di::discatier_engine_adapter
