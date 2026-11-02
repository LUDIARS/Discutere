#include "flow/user_voices/nfkc.hpp"

#include <algorithm>
#include <map>
#include <mutex>

#include "base/strings.hpp"

namespace di::flow::user_voices {
namespace {

struct NfkcEntry {
    std::uint32_t cp;
    std::uint32_t offset;
    std::uint32_t length;
};
struct NfkcCompose {
    std::uint32_t base;
    std::uint32_t mark;
    std::uint32_t composed;
};

#include "flow/user_voices/nfkc_table.inc"

bool is_combining(std::uint32_t cp) { return (cp >= 0x300 && cp <= 0x36F) || cp == 0x3099 || cp == 0x309A; }

const std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>& reverse_compose() {
    static const auto table = [] {
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> m;
        for (const auto& c : kNfkcCompose) m.emplace(c.composed, std::make_pair(c.base, c.mark));
        return m;
    }();
    return table;
}

bool is_cased_letter(std::uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7) ||
           (cp >= 0x370 && cp <= 0x3FF) || (cp >= 0x400 && cp <= 0x52F) || (cp >= 0x1E00 && cp <= 0x1FFF);
}

std::uint32_t lower_one(std::uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 32;
    if (cp >= 0x100 && cp <= 0x137 && cp % 2 == 0 && cp != 0x130) return cp + 1;
    if (cp >= 0x139 && cp <= 0x148 && cp % 2 == 1) return cp + 1;
    if (cp >= 0x14A && cp <= 0x177 && cp % 2 == 0) return cp + 1;
    if (cp == 0x178) return 0xFF;
    if (cp >= 0x179 && cp <= 0x17E && cp % 2 == 1) return cp + 1;
    if (cp == 0x386) return 0x3AC;
    if (cp >= 0x388 && cp <= 0x38A) return cp + 37;
    if (cp == 0x38C) return 0x3CC;
    if (cp == 0x38E || cp == 0x38F) return cp + 63;
    if ((cp >= 0x391 && cp <= 0x3A1) || (cp >= 0x3A3 && cp <= 0x3AB)) return cp + 32;
    if (cp >= 0x400 && cp <= 0x40F) return cp + 80;
    if (cp >= 0x410 && cp <= 0x42F) return cp + 32;
    if (((cp >= 0x460 && cp <= 0x481) || (cp >= 0x48A && cp <= 0x4BF)) && cp % 2 == 0) return cp + 1;
    if (cp == 0x4C0) return 0x4CF;
    if (cp >= 0x4C1 && cp <= 0x4CE && cp % 2 == 1) return cp + 1;
    if (cp >= 0x4D0 && cp <= 0x52F && cp % 2 == 0) return cp + 1;
    if (cp >= 0x1E00 && cp <= 0x1E95 && cp % 2 == 0) return cp + 1;
    if (cp == 0x1E9E) return 0xDF;
    if (cp >= 0x1EA0 && cp <= 0x1EFF && cp % 2 == 0) return cp + 1;
    if (cp >= 0x2160 && cp <= 0x216F) return cp + 16;  // ローマ数字
    if (cp >= 0x24B6 && cp <= 0x24CF) return cp + 26;  // 丸囲み英字
    if (cp >= 0xFF21 && cp <= 0xFF3A) return cp + 32;
    return cp;
}

} // namespace

std::vector<std::uint32_t> nfkc_char(std::uint32_t cp) {
    const auto* begin = std::begin(kNfkcMap);
    const auto* end = std::end(kNfkcMap);
    const auto* it = std::lower_bound(begin, end, cp, [](const NfkcEntry& e, std::uint32_t v) { return e.cp < v; });
    if (it == end || it->cp != cp) return {};
    return std::vector<std::uint32_t>(kNfkcPool + it->offset, kNfkcPool + it->offset + it->length);
}

std::optional<std::uint32_t> compose_pair(std::uint32_t base, std::uint32_t mark) {
    const auto* begin = std::begin(kNfkcCompose);
    const auto* end = std::end(kNfkcCompose);
    const auto* it = std::lower_bound(begin, end, std::make_pair(base, mark), [](const NfkcCompose& c, const auto& v) {
        return c.base < v.first || (c.base == v.first && c.mark < v.second);
    });
    if (it == end || it->base != base || it->mark != mark) return std::nullopt;
    return it->composed;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> decompose_pair(std::uint32_t composed) {
    const auto& table = reverse_compose();
    auto it = table.find(composed);
    if (it == table.end()) return std::nullopt;
    return it->second;
}

std::string nfkc_approx(std::string_view s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    for (std::uint32_t cp : str::codepoints(s)) {
        auto mapped = nfkc_char(cp);
        if (mapped.empty()) mapped.push_back(cp);
        for (std::uint32_t m : mapped) {
            if (is_combining(m) && !out.empty()) {
                if (auto c = compose_pair(out.back(), m)) {
                    out.back() = *c;
                    continue;
                }
            }
            out.push_back(m);
        }
    }
    return str::from_codepoints(out);
}

std::string js_to_lower(std::string_view s) {
    const auto cps = str::codepoints(s);
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < cps.size(); ++i) {
        const std::uint32_t cp = cps[i];
        if (cp == 0x130) {  // İ → i + U+0307
            str::append_utf8(out, 'i');
            str::append_utf8(out, 0x307);
            continue;
        }
        if (cp == 0x3A3) {  // Σ: 語末 (前が文字で後ろが文字でない) は ς
            const bool after_letter = i > 0 && is_cased_letter(cps[i - 1]);
            const bool before_letter = i + 1 < cps.size() && is_cased_letter(cps[i + 1]);
            str::append_utf8(out, after_letter && !before_letter ? 0x3C2 : 0x3C3);
            continue;
        }
        str::append_utf8(out, lower_one(cp));
    }
    return out;
}

} // namespace di::flow::user_voices
