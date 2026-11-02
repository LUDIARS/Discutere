#pragma once
// LLM 応答 JSON の値を JS の String(v) / Number(v) と同じ規則で読む小さな補助 (w3-M の局所の補助)。
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include "base/json.hpp"
#include "base/strings.hpp"

namespace di::persona_engine::facilitator {

/** JS の String(v) (null は "null"、配列は要素を "," で結合し null 要素は ""、object は "[object Object]")。 */
inline std::string js_string(const Json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "null";
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number()) return js_number_to_string(v.get<double>());
    if (v.is_array()) {
        std::string out;
        bool first = true;
        for (const auto& e : v) {
            if (!first) out += ",";
            first = false;
            if (!e.is_null()) out += js_string(e);
        }
        return out;
    }
    return "[object Object]";
}

/** String(obj[key] ?? fallback) */
inline std::string js_string_or(const Json& obj, std::string_view key, std::string_view fallback) {
    if (!obj.is_object()) return std::string(fallback);
    const auto it = obj.find(std::string(key));
    if (it == obj.end() || it->is_null()) return std::string(fallback);
    return js_string(*it);
}

/** JS の Number(v) (undefined / object は NaN、null は 0、真偽は 0/1、配列は String を経由)。 */
inline double js_number_of(const Json* v) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (!v) return nan;
    if (v->is_number()) return v->get<double>();
    if (v->is_null()) return 0;
    if (v->is_boolean()) return v->get<bool>() ? 1 : 0;
    if (v->is_string() || v->is_array()) {
        const auto n = str::js_number(v->is_string() ? v->get<std::string>() : js_string(*v));
        return n ? *n : nan;
    }
    return nan;
}

} // namespace di::persona_engine::facilitator
