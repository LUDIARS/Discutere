#include "lib/sentiment_core/js_number.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "base/strings.hpp"

namespace di::lib::sentiment_core {

std::string js_to_fixed(double value, int digits) {
    if (std::isnan(value)) return "NaN";
    if (digits < 0) digits = 0;
    if (digits > 100) digits = 100;
    if (std::fabs(value) >= 1e21) return js_number_to_string(value);
    const bool negative = value < 0;  // -0 は符号を付けない (JS と同じ)
    const double x = std::fabs(value);
    // 有限の double の 10 進展開は小数 1074 桁以内で終わるので、十分な桁数で書けば正確な展開になる。
    std::vector<char> buffer(1200);
    const int n = std::snprintf(buffer.data(), buffer.size(), "%.1100f", x);
    if (n <= 0) return js_number_to_string(value);
    std::string text(buffer.data(), static_cast<std::size_t>(n));
    const auto dot = text.find('.');
    std::string int_part = text.substr(0, dot);
    std::string frac = dot == std::string::npos ? std::string() : text.substr(dot + 1);
    std::string kept = int_part + frac.substr(0, static_cast<std::size_t>(digits));
    const char next = static_cast<std::size_t>(digits) < frac.size() ? frac[static_cast<std::size_t>(digits)] : '0';
    // 正確な展開なので「次の桁 >= 5」で切り上げ = 最も近い n、同点は大きい n (JS の規則)。
    if (next >= '5') {
        std::size_t i = kept.size();
        while (i > 0) {
            --i;
            if (kept[i] == '9') {
                kept[i] = '0';
                continue;
            }
            ++kept[i];
            break;
        }
        // 全桁が 9 だった (繰り上がりで全部 0 に戻った) ときは先頭に 1 を足す
        if (kept.find_first_not_of('0') == std::string::npos) kept.insert(kept.begin(), '1');
    }
    const std::size_t int_len = kept.size() - static_cast<std::size_t>(digits);
    std::string out = negative ? "-" : "";
    std::string ip = kept.substr(0, int_len);
    // 先頭の余分な 0 を落とす (整数部は最低 1 桁)
    std::size_t lead = 0;
    while (lead + 1 < ip.size() && ip[lead] == '0') ++lead;
    out += ip.substr(lead);
    if (digits > 0) out += "." + kept.substr(int_len);
    return out;
}

double js_round_fixed(double value, int digits) {
    if (!std::isfinite(value)) return value;
    const auto parsed = str::parse_double(js_to_fixed(value, digits));
    return parsed ? *parsed : std::numeric_limits<double>::quiet_NaN();
}

double js_to_number(const Json& value) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (value.is_number()) return value.get<double>();
    if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
    if (value.is_null()) return 0.0;
    if (value.is_string()) {
        auto n = str::js_number(value.get<std::string>());
        return n ? *n : nan;
    }
    if (value.is_array()) {
        if (value.empty()) return 0.0;
        if (value.size() == 1) {
            const Json& e = value[0];
            if (e.is_null()) return 0.0;              // String([null]) === ""
            if (e.is_boolean() || e.is_object()) return nan;  // "true" / "[object Object]"
            return js_to_number(e);
        }
        return nan;
    }
    return nan;
}

} // namespace di::lib::sentiment_core
