#include "core/projection/command_parser.hpp"

#include <cstdint>

#include "base/strings.hpp"

namespace di::core::projection {

std::optional<ParsedCommand> parse_command(std::string_view content) {
    const std::string text = str::trim(content);
    if (!str::starts_with(text, "/")) return std::nullopt;
    const std::string body = str::trim(std::string_view(text).substr(1));
    if (body.empty()) return std::nullopt;
    // body.split(/\s+/) — trim 済みなので空の要素は出ない
    std::vector<std::string> tokens;
    std::string current;
    std::size_t i = 0;
    while (i < body.size()) {
        const std::size_t start = i;
        const std::uint32_t cp = str::next_codepoint(body, i);
        if (str::is_js_space(cp)) {
            if (!current.empty()) tokens.push_back(std::move(current));
            current.clear();
        } else {
            current.append(body, start, i - start);
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    if (tokens.empty() || tokens[0].empty()) return std::nullopt;
    ParsedCommand out;
    out.command = str::to_lower(tokens[0]);
    out.args.assign(tokens.begin() + 1, tokens.end());
    return out;
}

} // namespace di::core::projection
