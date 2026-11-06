#include "flow/persona_import_document.hpp"

#include "base/strings.hpp"

namespace di::flow {

namespace {

std::vector<Json> as_personas(Json value) {
    if (value.is_array()) return std::vector<Json>(value.begin(), value.end());
    if (value.is_object()) {
        auto it = value.find("personas");
        if (it != value.end() && it->is_array()) return std::vector<Json>(it->begin(), it->end());
        return {std::move(value)};
    }
    return {std::move(value)};
}

} // namespace

JsonLimits persona_document_json_limits() { return JsonLimits{16u << 20, 2'000'000, 64}; }

PersonaDocument parse_persona_document(std::string_view text) {
    const std::string trimmed = str::trim(text);
    if (trimmed.empty()) return {};
    const JsonLimits limits = persona_document_json_limits();
    if (auto whole = parse_json(trimmed, limits)) return {as_personas(std::move(*whole)), 0};
    // 全体が JSON でなければ NDJSON: trimmed.split(/\r?\n/).filter(Boolean)
    PersonaDocument out;
    std::size_t start = 0;
    while (start <= trimmed.size()) {
        auto nl = trimmed.find('\n', start);
        std::size_t end = nl == std::string::npos ? trimmed.size() : nl;
        std::size_t line_end = end;
        if (line_end > start && trimmed[line_end - 1] == '\r') --line_end;
        const std::string_view line(trimmed.data() + start, line_end - start);
        if (!line.empty()) {
            if (auto v = parse_json(line, limits)) out.personas.push_back(std::move(*v));
            else out.invalidJsonLines += 1;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

} // namespace di::flow
