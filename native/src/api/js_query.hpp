#pragma once
// クエリ文字列を JS の Number(v) / Number.parseInt(v, 10) と同じ規則で読む (w4-Q の局所の補助)。
// learning-view / web-chat / blackbox のルートが使う。
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/strings.hpp"

namespace di::api::js_query {

/** Number.parseInt(v, 10): 先頭の空白・符号の後の 10 進数字の並び。数字が無ければ NaN (nullopt)。 */
inline std::optional<double> parse_int(std::string_view v) {
    std::size_t i = 0;
    // 先頭の空白 (JS の空白は ASCII 以外も含むので trim_start で落とす)
    const std::string s = str::trim_start(v);
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        ++i;
    }
    double n = 0;
    bool any = false;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        n = n * 10 + (s[i] - '0');
        any = true;
    }
    if (!any) return std::nullopt;
    return neg ? -n : n;
}

/** Number(v) (undefined は NaN = nullopt)。 */
inline std::optional<double> number(const std::optional<std::string>& v) {
    if (!v) return std::nullopt;
    const auto n = str::js_number(*v);
    if (!n || std::isnan(*n)) return std::nullopt;
    return n;
}

/** 整数値の double を int64 に (範囲外・非整数は nullopt)。 */
inline std::optional<std::int64_t> to_int64(double n) {
    if (!std::isfinite(n) || std::trunc(n) != n || n < -9.2e18 || n > 9.2e18) return std::nullopt;
    return static_cast<std::int64_t>(n);
}

} // namespace di::api::js_query
