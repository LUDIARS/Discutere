#pragma once
// String#localeCompare (Node の ICU、既定ロケール en-US) の近似。表示用の並べ替え (ペルソナ名・affect キー) に使う。
// ICU を持ち込まないための局所的な補助。CLDR root の照合を 3 段 (基本字 → アクセント・濁点 → 大小・かなの種別・幅) で近似する。
//   - 記号・空白 < 数字 < ラテン文字 < その他の文字 < かな < 漢字 (漢字は code point 順)
//   - 大小は小文字が先、全角/半角は後、ひらがなはカタカナより先、小書きかなは先
// 完全な ICU の照合ではない (差は port-notes/w1-F.md)。
#include <string_view>

namespace di::visualize {

/** a.localeCompare(b) の符号 (-1 / 0 / 1)。 */
int locale_compare(std::string_view a, std::string_view b);

/** JS の Array#sort の既定比較 (UTF-16 のコード単位の順)。 */
int js_default_compare(std::string_view a, std::string_view b);

} // namespace di::visualize
