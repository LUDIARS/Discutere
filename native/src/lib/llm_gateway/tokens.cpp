#include "lib/llm_gateway/tokens.hpp"

#include <cmath>
#include <cstdint>

#include "base/strings.hpp"

namespace di::lib::llm_gateway {

namespace {
/** CJK (漢字・かな・全角) 系コードポイントか。 */
bool is_cjk(std::uint32_t code) {
    return (code >= 0x3040 && code <= 0x30ff) ||  // ひらがな + カタカナ
           (code >= 0x3400 && code <= 0x4dbf) ||  // CJK 拡張 A
           (code >= 0x4e00 && code <= 0x9fff) ||  // CJK 統合漢字
           (code >= 0xf900 && code <= 0xfaff) ||  // CJK 互換漢字
           (code >= 0xff00 && code <= 0xffef);    // 全角英数・記号
}
} // namespace

long long estimate_tokens(std::string_view text) {
    if (text.empty()) return 0;
    double cjk = 0;
    double other = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::uint32_t cp = str::next_codepoint(text, i);
        if (is_cjk(cp)) cjk += 1;
        else other += 1;
    }
    return static_cast<long long>(std::ceil(cjk / 1.5 + other / 4));
}

} // namespace di::lib::llm_gateway
