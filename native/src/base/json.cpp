#include "base/json.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <system_error>

#include <lorica/input_budget.hpp>

namespace di {
namespace {

struct BudgetExceeded {
    const char* reason;
};

void append_escaped(std::string& out, std::string_view s) {
    out.push_back('"');
    // 不正な UTF-8 は U+FFFD に置き換える (JS の文字列は常に妥当な UTF-16 なので、ここで揃える)。
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
            }
            ++i;
            continue;
        }
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        bool valid = len != 0 && i + len <= s.size();
        for (std::size_t k = 1; valid && k < len; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) valid = false;
            else cp = (cp << 6) | (cc & 0x3F);
        }
        if (valid) {
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
                cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                valid = false;
        }
        if (valid) {
            out.append(s.substr(i, len));
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    out.push_back('"');
}

void append_number(std::string& out, const Json& v) {
    if (v.is_number_integer()) {
        if (v.is_number_unsigned()) out += std::to_string(v.get<std::uint64_t>());
        else out += std::to_string(v.get<std::int64_t>());
        return;
    }
    const double d = v.get<double>();
    if (!std::isfinite(d)) {
        out += "null";
        return;
    }
    out += js_number_to_string(d);
}

void write_value(std::string& out, const Json& v, int indent, int level) {
    const auto newline = [&](int lv) {
        if (indent <= 0) return;
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent * lv), ' ');
    };
    switch (v.type()) {
    case Json::value_t::null:
    case Json::value_t::discarded:
        out += "null";
        return;
    case Json::value_t::boolean:
        out += v.get<bool>() ? "true" : "false";
        return;
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
    case Json::value_t::number_float:
        append_number(out, v);
        return;
    case Json::value_t::string:
        append_escaped(out, v.get_ref<const std::string&>());
        return;
    case Json::value_t::binary:
        out += "null";
        return;
    case Json::value_t::array: {
        if (v.empty()) {
            out += "[]";
            return;
        }
        out.push_back('[');
        bool first = true;
        for (const auto& item : v) {
            if (!first) out.push_back(',');
            first = false;
            newline(level + 1);
            write_value(out, item, indent, level + 1);
        }
        newline(level);
        out.push_back(']');
        return;
    }
    case Json::value_t::object: {
        if (v.empty()) {
            out += "{}";
            return;
        }
        out.push_back('{');
        bool first = true;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (!first) out.push_back(',');
            first = false;
            newline(level + 1);
            append_escaped(out, it.key());
            out.push_back(':');
            if (indent > 0) out.push_back(' ');
            write_value(out, it.value(), indent, level + 1);
        }
        newline(level);
        out.push_back('}');
        return;
    }
    }
}

const Json* find_key(const Json& object, std::string_view key) {
    if (!object.is_object()) return nullptr;
    auto it = object.find(std::string(key));
    if (it == object.end()) return nullptr;
    return &*it;
}

} // namespace

std::optional<Json> parse_json(std::string_view text, const JsonLimits& limits, std::string* error) {
    lorica::input_budget budget({limits.max_bytes, limits.max_elements, limits.max_elements, limits.max_depth});
    if (!budget.consume(text.size(), 0, 0)) {
        if (error) *error = "json: input too large";
        return std::nullopt;
    }
    Json::parser_callback_t callback = [&budget](int depth, Json::parse_event_t event, Json&) -> bool {
        using E = Json::parse_event_t;
        if (event == E::object_start || event == E::array_start) {
            if (!budget.permits_depth(static_cast<std::size_t>(depth) + 1)) throw BudgetExceeded{"json: nesting too deep"};
        }
        if (event == E::object_start || event == E::array_start || event == E::value || event == E::key) {
            if (!budget.consume(0, 1, 1)) throw BudgetExceeded{"json: too many elements"};
        }
        return true;
    };
    try {
        Json parsed = Json::parse(text.begin(), text.end(), callback, true, false);
        return parsed;
    } catch (const BudgetExceeded& e) {
        if (error) *error = e.reason;
    } catch (const std::exception& e) {
        if (error) *error = std::string("json: ") + e.what();
    }
    return std::nullopt;
}

Json parse_json_or_throw(std::string_view text, const JsonLimits& limits) {
    std::string error;
    auto parsed = parse_json(text, limits, &error);
    if (!parsed) throw std::runtime_error(error);
    return std::move(*parsed);
}

std::string js_number_to_string(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
    if (value == 0) return "0";
    std::string out;
    if (value < 0) {
        out.push_back('-');
        value = -value;
    }
    std::array<char, 64> buf{};
    auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::scientific);
    if (res.ec != std::errc()) return "NaN";
    std::string_view sci(buf.data(), static_cast<std::size_t>(res.ptr - buf.data()));
    const auto epos = sci.find('e');
    std::string digits;
    for (char c : sci.substr(0, epos)) {
        if (c != '.') digits.push_back(c);
    }
    int exponent = 0;
    {
        std::string_view exp = sci.substr(epos + 1);
        bool negative = false;
        if (!exp.empty() && (exp.front() == '+' || exp.front() == '-')) {
            negative = exp.front() == '-';
            exp.remove_prefix(1);
        }
        for (char c : exp) exponent = exponent * 10 + (c - '0');
        if (negative) exponent = -exponent;
    }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    const int k = static_cast<int>(digits.size());
    const int n = exponent + 1;
    if (k <= n && n <= 21) {
        out += digits;
        out.append(static_cast<std::size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out += digits.substr(0, static_cast<std::size_t>(n));
        out.push_back('.');
        out += digits.substr(static_cast<std::size_t>(n));
    } else if (-6 < n && n <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-n), '0');
        out += digits;
    } else {
        out.push_back(digits[0]);
        if (k > 1) {
            out.push_back('.');
            out += digits.substr(1);
        }
        out.push_back('e');
        out.push_back(n - 1 >= 0 ? '+' : '-');
        out += std::to_string(n - 1 >= 0 ? n - 1 : -(n - 1));
    }
    return out;
}

std::string json_stringify(const Json& value) {
    std::string out;
    write_value(out, value, 0, 0);
    return out;
}

std::string json_stringify_pretty(const Json& value, int indent) {
    std::string out;
    write_value(out, value, indent, 0);
    return out;
}

std::string json_string(const Json& object, std::string_view key, std::string_view fallback) {
    const Json* v = find_key(object, key);
    if (v && v->is_string()) return v->get<std::string>();
    return std::string(fallback);
}

std::optional<std::string> json_opt_string(const Json& object, std::string_view key) {
    const Json* v = find_key(object, key);
    if (v && v->is_string()) return v->get<std::string>();
    return std::nullopt;
}

double json_number(const Json& object, std::string_view key, double fallback) {
    const Json* v = find_key(object, key);
    if (v && v->is_number()) return v->get<double>();
    return fallback;
}

std::optional<double> json_opt_number(const Json& object, std::string_view key) {
    const Json* v = find_key(object, key);
    if (v && v->is_number()) return v->get<double>();
    return std::nullopt;
}

std::int64_t json_int(const Json& object, std::string_view key, std::int64_t fallback) {
    const Json* v = find_key(object, key);
    if (v && v->is_number_integer()) return v->get<std::int64_t>();
    if (v && v->is_number_float()) {
        const double d = v->get<double>();
        if (std::isfinite(d) && d >= -9.2e18 && d <= 9.2e18) return static_cast<std::int64_t>(d);
    }
    return fallback;
}

bool json_bool(const Json& object, std::string_view key, bool fallback) {
    const Json* v = find_key(object, key);
    if (v && v->is_boolean()) return v->get<bool>();
    return fallback;
}

std::optional<bool> json_opt_bool(const Json& object, std::string_view key) {
    const Json* v = find_key(object, key);
    if (v && v->is_boolean()) return v->get<bool>();
    return std::nullopt;
}

const Json& json_array(const Json& object, std::string_view key) {
    static const Json empty = Json::array();
    const Json* v = find_key(object, key);
    if (v && v->is_array()) return *v;
    return empty;
}

const Json& json_object(const Json& object, std::string_view key) {
    static const Json empty = Json::object();
    const Json* v = find_key(object, key);
    if (v && v->is_object()) return *v;
    return empty;
}

bool json_truthy(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
    case Json::value_t::discarded: return false;
    case Json::value_t::boolean: return value.get<bool>();
    case Json::value_t::number_integer: return value.get<std::int64_t>() != 0;
    case Json::value_t::number_unsigned: return value.get<std::uint64_t>() != 0;
    case Json::value_t::number_float: {
        const double d = value.get<double>();
        return d != 0 && !std::isnan(d);
    }
    case Json::value_t::string: return !value.get_ref<const std::string&>().empty();
    default: return true;
    }
}

} // namespace di
