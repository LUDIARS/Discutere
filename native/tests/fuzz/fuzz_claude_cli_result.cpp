// `claude -p --output-format json` の標準出力の読み取り (parse_claude_cli_result) の fuzz の入口。
#include "fuzz_entry.hpp"

#include <string>

#include "persona_engine/llm/claude_cli.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string raw(reinterpret_cast<const char*>(data), size);
    (void)di::persona_engine::parse_claude_cli_result(raw, 1u << 16);
    return 0;
}
