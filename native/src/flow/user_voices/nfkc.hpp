#pragma once
// String#normalize("NFKC") と toLowerCase の近似 (ICU を持ち込まないための局所的な補助)。
// 表は native/tests/golden/w1-F-nfkc-table.gen.ts が Node から書き出した nfkc_table.inc。
//
// 近似の範囲 (完全な NFKC ではない。差は port-notes/w1-F.md):
//   - 1 文字ごとの互換分解・合成 (Node の NFKC(c) と同じ置き換え) を当てる。
//   - そのあと隣り合う「基底 + 結合文字」(Latin/Greek/Cyrillic/かな + U+0300..036F, U+3099, U+309A) だけ合成する。
//   - 結合文字の並べ替え (canonical ordering) と、ハングル字母の合成はしない。
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace di::flow::user_voices {

/** 1 文字の NFKC の置き換え (置き換えが無ければ空)。 */
std::vector<std::uint32_t> nfkc_char(std::uint32_t cp);
/** 基底 + 結合文字 → 合成済み 1 文字 (無ければ nullopt)。 */
std::optional<std::uint32_t> compose_pair(std::uint32_t base, std::uint32_t mark);
/** 合成済み 1 文字 → (基底, 結合文字) (表に無ければ nullopt)。 */
std::optional<std::pair<std::uint32_t, std::uint32_t>> decompose_pair(std::uint32_t composed);
/** s.normalize("NFKC") の近似。 */
std::string nfkc_approx(std::string_view s);
/** s.toLowerCase() の近似 (ASCII / Latin-1 / Latin 拡張 A・追加 / ギリシャ (語末シグマ) / キリル / 全角英字)。 */
std::string js_to_lower(std::string_view s);

} // namespace di::flow::user_voices
