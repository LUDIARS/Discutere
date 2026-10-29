#include "persona_engine/llm/model_spec.hpp"

#include "base/strings.hpp"

namespace di::persona_engine {
namespace {
// /^[a-z0-9][a-z0-9._:/-]*$/i
bool valid_model_id(const std::string& m) {
    if (m.empty()) return false;
    for (std::size_t i = 0; i < m.size(); ++i) {
        const char c = m[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (i == 0 ? !alnum : !(alnum || c == '.' || c == '_' || c == ':' || c == '/' || c == '-')) return false;
    }
    return true;
}
} // namespace

ModelSpec parse_model_spec(const std::optional<std::string>& spec) {
    if (!spec || spec->empty()) return {"", std::nullopt};
    const auto at = spec->rfind('@');
    if (at == std::string::npos || at == 0) return {str::trim(*spec), std::nullopt};
    ModelSpec out{str::trim(spec->substr(0, at)), std::nullopt};
    std::string effort = str::to_lower(str::trim(spec->substr(at + 1)));
    if (effort.empty()) return out;
    if (effort == "mid" || effort == "med") effort = "medium";
    out.effort = effort;
    return out;
}

std::optional<std::string> validate_model_spec(const ModelSpec& spec, ModelProvider provider) {
    if (spec.model.empty()) return std::string("model 未指定");
    if (!valid_model_id(spec.model)) return std::string("model に使用できない文字が含まれています");
    if (spec.effort) {
        const auto& e = *spec.effort;
        const bool ok = provider == ModelProvider::claude
                            ? (e == "low" || e == "medium" || e == "high" || e == "xhigh" || e == "max")
                            : (e == "minimal" || e == "low" || e == "medium" || e == "high" || e == "xhigh");
        if (!ok) return std::string(provider == ModelProvider::claude ? "claude" : "codex") + " で未対応の effort です";
    }
    return std::nullopt;
}

bool is_codex_model(const std::optional<std::string>& spec) {
    const auto m = str::to_lower_ascii(parse_model_spec(spec).model);
    return str::starts_with(m, "gpt-");
}

std::optional<std::string> model_of_spec(const std::optional<std::string>& spec) {
    auto m = parse_model_spec(spec).model;
    if (m.empty()) return std::nullopt;
    return m;
}

} // namespace di::persona_engine
