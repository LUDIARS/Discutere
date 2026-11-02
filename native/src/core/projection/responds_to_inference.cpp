#include "core/projection/responds_to_inference.hpp"

#include "base/strings.hpp"

namespace di::core::projection {

std::optional<std::string> infer_responds_to(std::string_view raw_content) {
    // rawContent.split(/\r?\n/, 1)[0].trim()
    std::size_t end = raw_content.find('\n');
    std::string_view first = raw_content.substr(0, end);
    if (end != std::string_view::npos && !first.empty() && first.back() == '\r') first.remove_suffix(1);
    const std::string line = str::trim(first);
    // /^>\s*@uid:([A-Za-z0-9._:-]+)/
    std::size_t i = 0;
    if (i >= line.size() || line[i] != '>') return std::nullopt;
    ++i;
    while (i < line.size()) {
        std::size_t next = i;
        if (!str::is_js_space(str::next_codepoint(line, next))) break;
        i = next;
    }
    if (line.compare(i, 5, "@uid:") != 0) return std::nullopt;
    i += 5;
    const std::size_t start = i;
    auto ok = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == ':' || c == '-';
    };
    while (i < line.size() && ok(line[i])) ++i;
    if (i == start) return std::nullopt;
    return line.substr(start, i - start);
}

} // namespace di::core::projection
