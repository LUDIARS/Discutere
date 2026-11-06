#include "flow/text_scan.hpp"

#include <cstdint>

#include "base/strings.hpp"

namespace di::flow::text_scan {

namespace {
bool is_open_quote(std::uint32_t cp) { return cp == U'「' || cp == U'『' || cp == '"' || cp == U'“'; }
bool is_close_quote(std::uint32_t cp) { return cp == U'」' || cp == U'』' || cp == '"' || cp == U'”'; }
} // namespace

std::vector<std::string> quoted_segments(std::string_view text, std::size_t min_units) {
    if (min_units == 0) min_units = 1;
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = i;
        const auto cp = str::next_codepoint(text, j);
        if (!is_open_quote(cp)) {
            i = j;
            continue;
        }
        // 開き記号の後、閉じ記号以外を数えて最初の閉じ記号まで進む
        std::size_t k = j;
        std::size_t units = 0;
        std::size_t close_at = std::string_view::npos;
        std::size_t after = k;
        while (k < text.size()) {
            std::size_t n = k;
            const auto c = str::next_codepoint(text, n);
            if (is_close_quote(c)) {
                close_at = k;
                after = n;
                break;
            }
            units += c > 0xFFFF ? 2 : 1;
            k = n;
        }
        if (close_at == std::string_view::npos) break;  // 以後に閉じ記号が無いので一致は起きない
        if (units < min_units) {                        // この位置では一致しない (次の文字から探す)
            i = j;
            continue;
        }
        out.emplace_back(text.substr(j, close_at - j));
        i = after;
    }
    return out;
}

std::string remove_js_spaces(std::string_view s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t start = i;
        const auto cp = str::next_codepoint(s, i);
        if (!str::is_js_space(cp)) out.append(s.substr(start, i - start));
    }
    return out;
}

bool is_evidence_id(std::string_view s) {
    if (s.size() < 2 || s.size() > 4 || s[0] != 'E') return false;
    for (std::size_t i = 1; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

} // namespace di::flow::text_scan
