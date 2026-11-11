// parse_json (HTTP 本文・WebSocket・DB の JSON 列・子プロセス出力が通る共通の JSON 読み取り) の fuzz の入口。
#include "fuzz_entry.hpp"

#include <string_view>

#include "base/json.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    // 小さな上限で読む (上限の境界を通りやすくする)。
    const di::JsonLimits limits{4096, 512, 16};
    std::string error;
    if (auto parsed = di::parse_json(text, limits, &error)) {
        (void)di::json_stringify(*parsed);
    }
    return 0;
}
