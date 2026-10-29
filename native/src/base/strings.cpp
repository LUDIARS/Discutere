#include "base/strings.hpp"

#include <charconv>
#include <cmath>
#include <limits>

namespace di::str {

std::uint32_t next_codepoint(std::string_view s, std::size_t& index) {
    const auto c = static_cast<unsigned char>(s[index]);
    if (c < 0x80) {
        ++index;
        return c;
    }
    std::size_t len = 0;
    std::uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    if (len == 0 || index + len > s.size()) {
        ++index;
        return 0xFFFD;
    }
    for (std::size_t k = 1; k < len; ++k) {
        const auto cc = static_cast<unsigned char>(s[index + k]);
        if ((cc & 0xC0) != 0x80) {
            ++index;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++index;
        return 0xFFFD;
    }
    index += len;
    return cp;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::vector<std::uint32_t> codepoints(std::string_view s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) out.push_back(next_codepoint(s, i));
    return out;
}

std::string from_codepoints(const std::vector<std::uint32_t>& cps) {
    std::string out;
    out.reserve(cps.size());
    for (auto cp : cps) append_utf8(out, cp);
    return out;
}

bool is_valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t before = i;
        const auto cp = next_codepoint(s, i);
        if (cp == 0xFFFD && !(i - before == 3 && s.substr(before, 3) == "\xEF\xBF\xBD")) return false;
    }
    return true;
}

std::string sanitize_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) append_utf8(out, next_codepoint(s, i));
    return out;
}

std::size_t js_length(std::string_view s) {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < s.size()) n += next_codepoint(s, i) >= 0x10000 ? 2 : 1;
    return n;
}

std::string js_slice(std::string_view s, std::int64_t start, std::optional<std::int64_t> end) {
    const auto len = static_cast<std::int64_t>(js_length(s));
    auto clamp = [len](std::int64_t v) {
        if (v < 0) v += len;
        if (v < 0) v = 0;
        if (v > len) v = len;
        return v;
    };
    const std::int64_t from = clamp(start);
    const std::int64_t to = end ? clamp(*end) : len;
    std::string out;
    if (from >= to) return out;
    std::int64_t pos = 0;
    std::size_t i = 0;
    while (i < s.size() && pos < to) {
        const std::size_t begin = i;
        const auto cp = next_codepoint(s, i);
        const std::int64_t units = cp >= 0x10000 ? 2 : 1;
        const std::int64_t unit_end = pos + units;
        if (pos >= from && unit_end <= to) {
            out.append(s.substr(begin, i - begin));
        } else if (unit_end > from && pos < to) {
            // サロゲートペアの片側だけが範囲に入る (JS では孤立サロゲート)。U+FFFD にする。
            append_utf8(out, 0xFFFD);
        }
        pos = unit_end;
    }
    return out;
}

std::string prefix_codepoints(std::string_view s, std::size_t n) {
    std::size_t i = 0;
    std::size_t count = 0;
    while (i < s.size() && count < n) {
        next_codepoint(s, i);
        ++count;
    }
    return std::string(s.substr(0, i));
}

std::size_t count_codepoints(std::string_view s) {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        next_codepoint(s, i);
        ++n;
    }
    return n;
}

bool is_js_space(std::uint32_t cp) {
    switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return true;
    default:
        return cp >= 0x2000 && cp <= 0x200A;
    }
}

std::string trim_start(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t next = i;
        if (!is_js_space(next_codepoint(s, next))) break;
        i = next;
    }
    return std::string(s.substr(i));
}

std::string trim_end(std::string_view s) {
    std::size_t i = 0;
    std::size_t last_non_space_end = 0;
    while (i < s.size()) {
        const auto cp = next_codepoint(s, i);
        if (!is_js_space(cp)) last_non_space_end = i;
    }
    return std::string(s.substr(0, last_non_space_end));
}

std::string trim(std::string_view s) { return trim_end(trim_start(s)); }

std::string collapse_whitespace(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool in_space = false;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t begin = i;
        const auto cp = next_codepoint(s, i);
        if (is_js_space(cp)) {
            if (!in_space) out.push_back(' ');
            in_space = true;
        } else {
            in_space = false;
            if (cp == 0xFFFD && i - begin == 1) append_utf8(out, cp);
            else out.append(s.substr(begin, i - begin));
        }
    }
    return out;
}

std::string squish(std::string_view s) { return trim(collapse_whitespace(s)); }

namespace {
std::uint32_t lower_cp(std::uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 0x20;
    if (cp < 0x80) return cp;
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;
    if (cp >= 0xFF21 && cp <= 0xFF3A) return cp + 0x20;
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 0x20;
    if (cp >= 0x410 && cp <= 0x42F) return cp + 0x20;
    if (cp >= 0x400 && cp <= 0x40F) return cp + 0x50;
    return cp;
}
std::uint32_t upper_cp(std::uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 0x20;
    if (cp < 0x80) return cp;
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 0x20;
    if (cp >= 0xFF41 && cp <= 0xFF5A) return cp - 0x20;
    if (cp >= 0x3B1 && cp <= 0x3C9 && cp != 0x3C2) return cp - 0x20;
    if (cp >= 0x430 && cp <= 0x44F) return cp - 0x20;
    if (cp >= 0x450 && cp <= 0x45F) return cp - 0x50;
    return cp;
}
template <class F>
std::string map_cps(std::string_view s, F f) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t begin = i;
        const auto cp = next_codepoint(s, i);
        const auto mapped = f(cp);
        if (mapped == cp && !(cp == 0xFFFD && i - begin == 1)) out.append(s.substr(begin, i - begin));
        else append_utf8(out, mapped);
    }
    return out;
}
} // namespace

std::string to_lower(std::string_view s) { return map_cps(s, lower_cp); }
std::string to_upper(std::string_view s) { return map_cps(s, upper_cp); }

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
    return out;
}

bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 0x20);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 0x20);
        if (x != y) return false;
    }
    return true;
}

std::vector<std::string> split(std::string_view s, std::string_view separator) {
    std::vector<std::string> out;
    if (separator.empty()) {
        // JS の "abc".split("") は UTF-16 単位だが、ここでは code point 単位にする。
        std::size_t i = 0;
        while (i < s.size()) {
            const std::size_t begin = i;
            next_codepoint(s, i);
            out.emplace_back(s.substr(begin, i - begin));
        }
        return out;
    }
    std::size_t start = 0;
    while (true) {
        const auto pos = s.find(separator, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + separator.size();
    }
    return out;
}

std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' || s[i] == '\r') {
            out.emplace_back(s.substr(start, i - start));
            if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ++i;
            start = i + 1;
        }
    }
    out.emplace_back(s.substr(start));
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out.append(separator);
        out.append(parts[i]);
    }
    return out;
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string(s);
    std::string out;
    out.reserve(s.size());
    std::size_t start = 0;
    while (true) {
        const auto pos = s.find(from, start);
        if (pos == std::string_view::npos) break;
        out.append(s.substr(start, pos - start));
        out.append(to);
        start = pos + from.size();
    }
    out.append(s.substr(start));
    return out;
}

bool contains(std::string_view s, std::string_view needle) { return s.find(needle) != std::string_view::npos; }
bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }
bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string repeat(std::string_view s, std::size_t times) {
    std::string out;
    out.reserve(s.size() * times);
    for (std::size_t i = 0; i < times; ++i) out.append(s);
    return out;
}

std::string pad_start(std::string_view s, std::size_t width, char fill) {
    const auto n = js_length(s);
    if (n >= width) return std::string(s);
    return std::string(width - n, fill) + std::string(s);
}

std::optional<std::int64_t> parse_int(std::string_view s) {
    if (s.empty()) return std::nullopt;
    std::int64_t value = 0;
    const char* begin = s.data();
    if (s.front() == '+') ++begin;
    auto res = std::from_chars(begin, s.data() + s.size(), value);
    if (res.ec != std::errc() || res.ptr != s.data() + s.size()) return std::nullopt;
    return value;
}

std::optional<double> parse_double(std::string_view s) {
    if (s.empty()) return std::nullopt;
    double value = 0;
    const char* begin = s.data();
    if (s.front() == '+') ++begin;
    auto res = std::from_chars(begin, s.data() + s.size(), value);
    if (res.ec != std::errc() || res.ptr != s.data() + s.size()) return std::nullopt;
    return value;
}

std::optional<double> js_number(std::string_view raw) {
    const std::string t = trim(raw);
    if (t.empty()) return 0.0;
    std::string_view s = t;
    if (s == "Infinity" || s == "+Infinity") return std::numeric_limits<double>::infinity();
    if (s == "-Infinity") return -std::numeric_limits<double>::infinity();
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O')) {
        const int base = (s[1] == 'x' || s[1] == 'X') ? 16 : (s[1] == 'b' || s[1] == 'B') ? 2 : 8;
        double v = 0;
        for (char c : s.substr(2)) {
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return std::nullopt;
            if (d >= base) return std::nullopt;
            v = v * base + d;
        }
        return v;
    }
    // [+-]? (digits [. digits?] | . digits) ([eE][+-]?digits)?
    std::size_t i = 0;
    if (s[i] == '+' || s[i] == '-') ++i;
    std::size_t int_digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++int_digits; }
    std::size_t frac_digits = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++frac_digits; }
    }
    if (int_digits == 0 && frac_digits == 0) return std::nullopt;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        std::size_t exp_digits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++exp_digits; }
        if (exp_digits == 0) return std::nullopt;
    }
    if (i != s.size()) return std::nullopt;
    double value = 0;
    std::string normalized(s);
    if (!normalized.empty() && normalized.front() == '+') normalized.erase(0, 1);
    if (!normalized.empty() && normalized.back() == '.') normalized.pop_back();
    auto res = std::from_chars(normalized.data(), normalized.data() + normalized.size(), value);
    if (res.ec == std::errc::result_out_of_range) {
        return normalized.front() == '-' ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    }
    if (res.ec != std::errc()) return std::nullopt;
    return value;
}

std::string escape_html(std::string_view s) {
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

std::string strip_control(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 && c != '\n' && c != '\t') continue;
        if (u == 0x7F) continue;
        out.push_back(c);
    }
    return out;
}

std::string escape_like(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '%' || c == '_' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

namespace {

char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 0x20) : c; }

bool istarts_at(std::string_view s, std::size_t pos, std::string_view needle) {
    if (pos + needle.size() > s.size()) return false;
    for (std::size_t k = 0; k < needle.size(); ++k)
        if (lower_ascii(s[pos + k]) != needle[k]) return false;
    return true;
}

std::size_t ifind(std::string_view s, std::string_view needle, std::size_t from) {
    if (needle.empty()) return from;
    for (std::size_t i = from; i + needle.size() <= s.size(); ++i) {
        if (lower_ascii(s[i]) == needle[0] && istarts_at(s, i, needle)) return i;
    }
    return std::string_view::npos;
}

bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// /<tag\b[^>]*>[\s\S]*?<\/tag>/gi → " " (線形)。
std::string remove_blocks(std::string_view s, std::string_view tag) {
    const std::string open = "<" + std::string(tag);
    const std::string close = "</" + std::string(tag) + ">";
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto lt = s.find('<', i);
        if (lt == std::string_view::npos) break;
        if (istarts_at(s, lt, open) && lt + open.size() < s.size() && !is_word_char(s[lt + open.size()])) {
            const auto gt = s.find('>', lt + open.size());
            if (gt == std::string_view::npos) break;                    // 以後 '>' が無いので一致は起きない
            const auto end = ifind(s, close, gt + 1);
            if (end == std::string_view::npos) break;                   // 以後に閉じタグが無いので一致は起きない
            out.append(s.substr(i, lt - i));
            out.push_back(' ');
            i = end + close.size();
            continue;
        }
        out.append(s.substr(i, lt + 1 - i));
        i = lt + 1;
    }
    out.append(s.substr(i));
    return out;
}

// /<[^>]+>/g → " " (線形)。
std::string remove_tags(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto lt = s.find('<', i);
        if (lt == std::string_view::npos) break;
        if (lt + 1 < s.size() && s[lt + 1] != '>') {
            const auto gt = s.find('>', lt + 1);
            if (gt == std::string_view::npos) break;
            out.append(s.substr(i, lt - i));
            out.push_back(' ');
            i = gt + 1;
            continue;
        }
        out.append(s.substr(i, lt + 1 - i));
        i = lt + 1;
    }
    out.append(s.substr(i));
    return out;
}

} // namespace

std::string decode_entities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto amp = s.find('&', i);
        if (amp == std::string_view::npos) break;
        out.append(s.substr(i, amp - i));
        // 実体名は短いので、';' を探すのは最大 32 文字先までにする (線形を保つ)。
        std::size_t semi = std::string_view::npos;
        for (std::size_t k = amp + 1; k < s.size() && k <= amp + 32; ++k) {
            if (s[k] == ';') {
                semi = k;
                break;
            }
            if (s[k] == '&') break;
        }
        if (semi == std::string_view::npos) {
            out.push_back('&');
            i = amp + 1;
            continue;
        }
        const std::string name = to_lower_ascii(s.substr(amp + 1, semi - amp - 1));
        bool ok = true;
        if (name == "amp") out.push_back('&');
        else if (name == "lt") out.push_back('<');
        else if (name == "gt") out.push_back('>');
        else if (name == "quot") out.push_back('"');
        else if (name == "apos") out.push_back('\'');
        else if (name == "nbsp") out.push_back(' ');
        else if (name.size() > 2 && name[0] == '#' && name[1] == 'x') {
            std::uint32_t cp = 0;
            for (char c : std::string_view(name).substr(2)) {
                if (c >= '0' && c <= '9') cp = cp * 16 + static_cast<std::uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') cp = cp * 16 + static_cast<std::uint32_t>(c - 'a' + 10);
                else { ok = false; break; }
                if (cp > 0x10FFFF) cp = 0x110000;
            }
            if (ok) append_utf8(out, cp);
        } else if (name.size() > 1 && name[0] == '#') {
            std::uint32_t cp = 0;
            for (char c : std::string_view(name).substr(1)) {
                if (c >= '0' && c <= '9') cp = cp * 10 + static_cast<std::uint32_t>(c - '0');
                else { ok = false; break; }
                if (cp > 0x10FFFF) cp = 0x110000;
            }
            if (ok) append_utf8(out, cp);
        } else {
            ok = false;
        }
        if (ok) {
            i = semi + 1;
        } else {
            out.push_back('&');
            i = amp + 1;
        }
    }
    out.append(s.substr(i));
    return out;
}

std::string strip_html(std::string_view html) {
    std::string a = remove_blocks(html, "script");
    std::string b = remove_blocks(a, "style");
    std::string c = remove_tags(b);
    return squish(decode_entities(c));
}

} // namespace di::str
