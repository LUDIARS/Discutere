// front matter (YAML の部分集合。攻略本・ゲームデータの md が通る独自の読み取り) の fuzz の入口。
#include "fuzz_entry.hpp"

#include <string_view>

#include "lib/front_matter/front_matter.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    di::lib::front_matter::Limits limits;
    limits.max_bytes = 1u << 16;
    limits.max_depth = 8;
    limits.max_elements = 1000;
    if (auto m = di::lib::front_matter::parse(text, nullptr, limits)) {
        (void)di::lib::front_matter::stringify(m->content, m->data);
    }
    return 0;
}
