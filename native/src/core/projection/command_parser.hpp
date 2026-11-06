#pragma once
// "/command args..." の解析 (src/core/projection/command-parser.ts)。
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace di::core::projection {

struct ParsedCommand {
    std::string command;
    std::vector<std::string> args;
};

/** "/" で始まらない・本文が空なら nullopt。command は小文字化。 */
std::optional<ParsedCommand> parse_command(std::string_view content);

} // namespace di::core::projection
