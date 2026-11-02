#pragma once
// flow の移植で正規表現の代わりに使う線形の走査 (w1-E のモジュール内の補助。std::regex は使わない)。
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace di::flow::text_scan {

/**
 * /[「『"“]([^」』"”]{min,})[」』"”]/g の一致の中身 (m[1]) を出現順に返す。
 * min は UTF-16 のコード単位数 (JS の量指定子と同じ数え方)。
 */
std::vector<std::string> quoted_segments(std::string_view text, std::size_t min_units);

/** s.replace(/\s+/g, "") (JS の空白をすべて落とす)。 */
std::string remove_js_spaces(std::string_view s);

/** /^E\d{1,3}$/ */
bool is_evidence_id(std::string_view s);

} // namespace di::flow::text_scan
