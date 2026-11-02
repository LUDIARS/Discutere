#include "visualize/locale_compare.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "base/strings.hpp"
#include "flow/user_voices/nfkc.hpp"

namespace di::visualize {
namespace {

struct Element {
    std::uint64_t primary = 0;
    std::uint32_t secondary = 0;
    std::uint32_t tertiary = 0;
};

enum Class : std::uint64_t { kSpace = 1, kSymbol = 2, kDigit = 3, kLatin = 4, kOtherScript = 5, kKana = 6, kHan = 7 };

std::uint64_t make_primary(Class c, std::uint64_t value) { return (static_cast<std::uint64_t>(c) << 40) | value; }

/** Node (ICU) で並べた記号の順。同じ文字列内の文字は基本字が同じ (第 3 段だけ違う)。 */
const std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>& symbol_order() {
    static const auto table = [] {
        const std::vector<std::string> groups{
            "\t", " 　", "_", "-", "‐", "–", "—", "―", "〜", "〰", "・", ",", "、", ";",
            ":", "!", "¡", "?", "¿", ".", "‥", "…", "。", "·", "'‘’",
            "\"“”〝〞〟", "«", "»", "(", ")", "[", "]", "{", "}", "〈", "〉", "《",
            "》", "「", "」", "『", "』", "【", "】", "〔", "〕", "〖", "〗",
            "〘", "〙", "〚", "〛", "§", "¶", "@", "*", "/", "\\", "&", "#", "%", "〃", "〽",
            "※", "`", "^", "°", "©", "®", "←", "→", "↑", "↓", "+", "±", "÷",
            "×", "<", "=", ">", "|", "~", "■", "□", "▲", "△", "▼", "▽", "◆", "◇",
            "○", "◎", "●", "★", "☆", "♀", "♂", "♪", "〄", "〒〶", "〓",
            "〠", "〷", "〾", "〿", "々", "〻", "〱〲", "〳〴", "〵", "ー",
            "$", "£", "¥", "€",
        };
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> m;
        std::uint32_t rank = 1;
        for (const auto& g : groups) {
            std::uint32_t tert = 0;
            for (std::uint32_t cp : str::codepoints(g)) m.emplace(cp, std::make_pair(rank, tert++));
            ++rank;
        }
        return m;
    }();
    return table;
}

bool is_small_kana(std::uint32_t h) {
    switch (h) {
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063: case 0x3083: case 0x3085: case 0x3087:
    case 0x308E: case 0x3095: case 0x3096:
        return true;
    default:
        return false;
    }
}

std::uint32_t large_kana(std::uint32_t h) {
    if (h == 0x3095) return 0x304B;
    if (h == 0x3096) return 0x3051;
    return h + 1;
}

bool is_han(std::uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x3FFFF);
}

bool is_symbol_like(std::uint32_t cp) {
    return cp < 0x80 || (cp >= 0xA0 && cp <= 0xBF) || cp == 0xD7 || cp == 0xF7 || (cp >= 0x2000 && cp <= 0x2BFF) ||
           (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0x1F000 && cp <= 0x1FAFF);
}

std::vector<Element> elements(std::string_view s) {
    std::vector<Element> out;
    // 合成済みの文字は基底 + 結合文字に分ける (é → e + U+0301、が → か + U+3099)。
    std::vector<std::pair<std::uint32_t, bool>> cps;  // (code point, 幅の違う形だったか)
    auto push_decomposed = [&](std::uint32_t cp, bool width) {
        std::vector<std::uint32_t> stack;
        std::uint32_t cur = cp;
        while (auto d = flow::user_voices::decompose_pair(cur)) {
            stack.push_back(d->second);
            cur = d->first;
        }
        cps.emplace_back(cur, width);
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) cps.emplace_back(*it, false);
    };
    for (std::uint32_t cp : str::codepoints(s)) {
        if (cp >= 0xFF01 && cp <= 0xFF5E) push_decomposed(cp - 0xFEE0, true);
        else if (cp >= 0xFF61 && cp <= 0xFF9F) {
            for (std::uint32_t m : flow::user_voices::nfkc_char(cp)) push_decomposed(m, m != 0x3099 && m != 0x309A);
        } else push_decomposed(cp, false);
    }
    for (const auto& [cp, width] : cps) {
        if ((cp >= 0x300 && cp <= 0x36F) || cp == 0x3099 || cp == 0x309A) {
            if (!out.empty()) {
                const std::uint32_t weight = cp == 0x3099 ? 1 : cp == 0x309A ? 2 : 3 + (cp - 0x300);
                out.back().secondary = out.back().secondary * 256 + weight;
            }
            continue;
        }
        Element e;
        const std::uint32_t w = width ? 1 : 0;
        if (str::is_js_space(cp) && !symbol_order().count(cp)) {
            e.primary = make_primary(kSpace, 0);
            e.tertiary = w;
        } else if (auto it = symbol_order().find(cp); it != symbol_order().end()) {
            e.primary = make_primary(it->second.first == 1 || it->second.first == 2 ? kSpace : kSymbol, it->second.first);
            e.tertiary = it->second.second * 2 + w;
        } else if (cp >= '0' && cp <= '9') {
            e.primary = make_primary(kDigit, cp - '0');
            e.tertiary = w;
        } else if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) {
            const bool upper = cp <= 'Z';
            e.primary = make_primary(kLatin, (upper ? cp + 32 : cp) - 'a');
            e.tertiary = (upper ? 2 : 0) + w;
        } else if ((cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30F6)) {
            const bool kata = cp >= 0x30A1;
            const std::uint32_t h = kata ? cp - 0x60 : cp;
            const bool small = is_small_kana(h);
            e.primary = make_primary(kKana, small ? large_kana(h) : h);
            e.tertiary = (small ? 0 : 1) + (kata ? 2 : 0) + (width ? 4 : 0);
        } else if (is_han(cp)) {
            e.primary = make_primary(kHan, cp);
        } else if (is_symbol_like(cp)) {
            e.primary = make_primary(kSymbol, 1000 + cp);
        } else {
            // その他の文字 (ラテン拡張・ギリシャ・キリル・ハングル等): 小文字に寄せて code point 順
            const std::string lower = flow::user_voices::js_to_lower(str::from_codepoints({cp}));
            const auto lc = str::codepoints(lower);
            const std::uint32_t base = lc.empty() ? cp : lc[0];
            if (cp >= 0xC0 && cp <= 0x24F) e.primary = make_primary(kLatin, 1000 + base);
            else e.primary = make_primary(kOtherScript, base);
            e.tertiary = (base != cp ? 2 : 0) + w;
        }
        out.push_back(e);
    }
    return out;
}

template <class F>
int compare_level(const std::vector<Element>& a, const std::vector<Element>& b, F key) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto ka = key(a[i]);
        const auto kb = key(b[i]);
        if (ka != kb) return ka < kb ? -1 : 1;
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return 0;
}

std::vector<std::uint16_t> utf16(std::string_view s) {
    std::vector<std::uint16_t> out;
    for (std::uint32_t cp : str::codepoints(s)) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<std::uint16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<std::uint16_t>(cp));
        }
    }
    return out;
}

} // namespace

int locale_compare(std::string_view a, std::string_view b) {
    if (a == b) return 0;
    const auto ea = elements(a);
    const auto eb = elements(b);
    if (int c = compare_level(ea, eb, [](const Element& e) { return e.primary; })) return c;
    if (int c = compare_level(ea, eb, [](const Element& e) { return e.secondary; })) return c;
    if (int c = compare_level(ea, eb, [](const Element& e) { return e.tertiary; })) return c;
    return 0;
}

int js_default_compare(std::string_view a, std::string_view b) {
    const auto ua = utf16(a);
    const auto ub = utf16(b);
    if (ua == ub) return 0;
    return ua < ub ? -1 : 1;
}

} // namespace di::visualize
