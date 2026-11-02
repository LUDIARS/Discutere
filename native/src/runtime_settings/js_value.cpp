#include "runtime_settings/js_value.hpp"

#include <cmath>
#include <limits>

#include "base/strings.hpp"

namespace di::runtime_settings::js {

std::string to_string(const Json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "null";
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_number_integer() || value.is_number_unsigned()) return json_stringify(value);
    if (value.is_number_float()) return js_number_to_string(value.get<double>());
    if (value.is_array()) {
        std::string out;
        bool first = true;
        for (const auto& item : value) {
            if (!first) out += ',';
            first = false;
            if (!item.is_null()) out += to_string(item);
        }
        return out;
    }
    return "[object Object]";
}

double to_number(const Json& value) {
    if (value.is_null()) return 0;
    if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
    if (value.is_number()) return value.get<double>();
    if (value.is_string() || value.is_array()) {
        auto n = str::js_number(to_string(value));
        return n ? *n : std::numeric_limits<double>::quiet_NaN();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

bool truthy(const Json& value) { return json_truthy(value); }

double round(double n) {
    if (!std::isfinite(n) || n == std::floor(n)) return n;
    return std::floor(n + 0.5);
}

} // namespace di::runtime_settings::js
