#include "design_diagnostic/validation.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "base/strings.hpp"

namespace di::design_diagnostic {

void fail(const std::string& code, const std::string& path, const std::string& reason) {
    throw DiagnosticError(code, path + ": " + reason);
}

const Json& object(const Json& value, const std::string& path, const std::vector<std::string>& keys, const std::string& code) {
    if (!value.is_object()) fail(code, path, "expected object");
    for (const auto& [key, v] : value.items()) {
        (void)v;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) fail(code, path, "unexpected field");
    }
    return value;
}

std::string string(const Json& value, const std::string& path, std::size_t max, const std::string& code) {
    if (!value.is_string()) fail(code, path, "expected nonempty string <= " + std::to_string(max) + " characters");
    const auto& s = value.get_ref<const std::string&>();
    if (str::trim(s).empty() || str::js_length(s) > max) fail(code, path, "expected nonempty string <= " + std::to_string(max) + " characters");
    return s;
}

const Json& array(const Json& value, const std::string& path, std::size_t max, const std::string& code, std::size_t min) {
    if (!value.is_array() || value.size() < min || value.size() > max)
        fail(code, path, "expected " + std::to_string(min) + ".." + std::to_string(max) + " items");
    return value;
}

double finite(const Json& value, const std::string& path, const std::string& code) {
    if (!value.is_number()) fail(code, path, "expected finite number");
    const double d = value.get<double>();
    if (!std::isfinite(d)) fail(code, path, "expected finite number");
    return d;
}

std::string choice(const Json& value, const std::vector<std::string>& choices, const std::string& path, const std::string& code) {
    if (!value.is_string() || std::find(choices.begin(), choices.end(), value.get<std::string>()) == choices.end())
        fail(code, path, "expected " + str::join(choices, " / "));
    return value.get<std::string>();
}

std::string id(const Json& value, const std::string& path, const std::string& code) {
    const std::string result = string(value, path, 80, code);
    auto alnum = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    bool ok = !result.empty() && alnum(result.front());
    for (char c : result)
        if (!alnum(c) && c != '_' && c != '.' && c != ':' && c != '-') ok = false;
    if (!ok) fail(code, path, "invalid ID");
    return result;
}

void unique(const std::vector<std::string>& values, const std::string& path, const std::string& code) {
    if (std::set<std::string>(values.begin(), values.end()).size() != values.size()) fail(code, path, "duplicate ID");
}

} // namespace di::design_diagnostic
