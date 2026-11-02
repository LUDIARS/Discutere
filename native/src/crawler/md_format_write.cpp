// stringify_game_kg は lib::front_matter::stringify (別担当が実装) を使うので、parse と別の翻訳単位に置く
// (静的ライブラリなので、書き出しを使わない実行ファイルは stringify を要求しない)。
#include "crawler/md_format.hpp"
#include "lib/front_matter/front_matter.hpp"

namespace di::crawler {

std::string stringify_game_kg(const GameKG& kg) {
    return lib::front_matter::stringify(kg.body.value_or(""), game_kg_frontmatter(kg));
}

} // namespace di::crawler
