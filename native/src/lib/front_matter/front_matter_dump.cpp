// front matter の書き (gray-matter 4.0.3 の matter.stringify と js-yaml 3.15.2 の safeDump)。
// 読み (parse) は front_matter_parse.cpp。
//
// js-yaml の dumper.js を JSON で表せる値 (object/array/string/number/bool/null) の範囲で写す。
// js-yaml は文字列を UTF-16 のコード単位で調べる。BMP 外の文字 (絵文字等) はサロゲートとなり
// isPrintable が偽になるので、その文字列は必ず二重引用符 (\UXXXXXXXX) になる。
// それ以外の経路では全文字が BMP なので、コード単位 = code point として扱ってよい。
#include "lib/front_matter/front_matter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/strings.hpp"

namespace di::lib::front_matter {
namespace {

using Cps = std::vector<std::uint32_t>;

constexpr int kIndent = 2;       // state.indent
constexpr int kLineWidth = 80;   // state.lineWidth
constexpr std::size_t kMaxDepth = 256;  // 入れ子の上限 (呼び出し側の Json は信頼するが、再帰で落ちないように)

// ─── 文字の分類 (dumper.js と同じ。c は UTF-16 のコード単位相当) ─────────────

bool is_whitespace(std::uint32_t c) { return c == 0x20 || c == 0x09; }

bool is_printable(std::uint32_t c) {
    // BMP 外は JS ではサロゲート (0xD800-0xDFFF) に見えるので印字不可。
    return (0x20 <= c && c <= 0x7E) || (0xA1 <= c && c <= 0xD7FF && c != 0x2028 && c != 0x2029) ||
           (0xE000 <= c && c <= 0xFFFD && c != 0xFEFF);
}

bool is_ns_char(std::uint32_t c) {
    return is_printable(c) && !is_whitespace(c) && c != 0xFEFF && c != 0x0D && c != 0x0A;
}

// prev == 0 は「前の文字が無い」(JS の null/0 は偽)。
bool is_plain_safe(std::uint32_t c, std::uint32_t prev) {
    return is_printable(c) && c != 0xFEFF && c != ',' && c != '[' && c != ']' && c != '{' && c != '}' &&
           c != ':' && (c != '#' || (prev != 0 && is_ns_char(prev)));
}

bool is_plain_safe_first(std::uint32_t c) {
    if (!is_printable(c) || c == 0xFEFF || is_whitespace(c)) return false;
    switch (c) {
    case '-': case '?': case ':': case ',': case '[': case ']': case '{': case '}': case '#': case '&':
    case '*': case '!': case '|': case '=': case '>': case '\'': case '"': case '%': case '@': case '`':
        return false;
    default:
        return true;
    }
}

// ─── 暗黙の型として読めてしまう文字列か (safe schema の resolve) ─────────────

bool dec(char c) { return c >= '0' && c <= '9'; }
bool oct(char c) { return c >= '0' && c <= '7'; }
bool hex(char c) { return dec(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }

bool resolve_null(const std::string& s) { return s == "~" || s == "null" || s == "Null" || s == "NULL"; }

bool resolve_bool(const std::string& s) {
    return s == "true" || s == "True" || s == "TRUE" || s == "false" || s == "False" || s == "FALSE";
}

// /^(:[0-5]?[0-9])+$/
bool base60_tail(const std::string& s, std::size_t i) {
    if (i >= s.size()) return false;
    while (i < s.size()) {
        if (s[i] != ':') return false;
        ++i;
        if (i < s.size() && s[i] >= '0' && s[i] <= '5' && i + 1 < s.size() && dec(s[i + 1])) i += 2;
        else if (i < s.size() && dec(s[i])) i += 1;
        else return false;
    }
    return true;
}

bool resolve_int(const std::string& data) {
    const std::size_t max = data.size();
    if (max == 0) return false;
    std::size_t index = 0;
    bool has_digits = false;
    auto at = [&](std::size_t i) -> char { return i < max ? data[i] : '\0'; };
    char ch = at(index);
    if (ch == '-' || ch == '+') ch = at(++index);
    if (ch == '0') {
        if (index + 1 == max) return true;
        ch = at(++index);
        if (ch == 'b') {
            ++index;
            for (; index < max; ++index) {
                ch = data[index];
                if (ch == '_') continue;
                if (ch != '0' && ch != '1') return false;
                has_digits = true;
            }
            return has_digits && ch != '_';
        }
        if (ch == 'x') {
            ++index;
            for (; index < max; ++index) {
                ch = data[index];
                if (ch == '_') continue;
                if (!hex(ch)) return false;
                has_digits = true;
            }
            return has_digits && ch != '_';
        }
        for (; index < max; ++index) {
            ch = data[index];
            if (ch == '_') continue;
            if (!oct(ch)) return false;
            has_digits = true;
        }
        return has_digits && ch != '_';
    }
    if (ch == '_') return false;
    for (; index < max; ++index) {
        ch = data[index];
        if (ch == '_') continue;
        if (ch == ':') break;
        if (!dec(ch)) return false;
        has_digits = true;
    }
    if (!has_digits || ch == '_') return false;
    if (ch != ':') return true;
    return base60_tail(data, index);
}

// YAML_FLOAT_PATTERN (js-yaml type/float.js) を線形に照合する。
bool float_pattern(const std::string& s) {
    const std::size_t n = s.size();
    auto digits_us = [&](std::size_t& i) {  // [0-9_]*
        while (i < n && (dec(s[i]) || s[i] == '_')) ++i;
    };
    auto exponent = [&](std::size_t& i) -> bool {  // (?:[eE][-+]?[0-9]+)? の後に終端
        if (i == n) return true;
        if (s[i] != 'e' && s[i] != 'E') return false;
        ++i;
        if (i < n && (s[i] == '-' || s[i] == '+')) ++i;
        const std::size_t start = i;
        while (i < n && dec(s[i])) ++i;
        return i > start && i == n;
    };
    // 1) [-+]?(?:0|[1-9][0-9_]*)(?:\.[0-9_]*)?(?:[eE][-+]?[0-9]+)?
    {
        std::size_t i = 0;
        if (i < n && (s[i] == '-' || s[i] == '+')) ++i;
        bool ok = false;
        if (i < n && s[i] == '0') {
            ++i;
            ok = true;
        } else if (i < n && s[i] >= '1' && s[i] <= '9') {
            ++i;
            digits_us(i);
            ok = true;
        }
        if (ok) {
            if (i < n && s[i] == '.') {
                ++i;
                digits_us(i);
            }
            if (exponent(i)) return true;
        }
    }
    // 2) \.[0-9_]+(?:[eE][-+]?[0-9]+)?
    if (n > 0 && s[0] == '.') {
        std::size_t i = 1;
        digits_us(i);
        if (i > 1 && exponent(i)) return true;
    }
    // 3) [-+]?[0-9][0-9_]*(?::[0-5]?[0-9])+\.[0-9_]*
    {
        std::size_t i = 0;
        if (i < n && (s[i] == '-' || s[i] == '+')) ++i;
        if (i < n && dec(s[i])) {
            ++i;
            digits_us(i);
            int groups = 0;
            while (i < n && s[i] == ':') {
                ++i;
                if (i + 1 < n && s[i] >= '0' && s[i] <= '5' && dec(s[i + 1])) {
                    // 2 桁で読むと続きが ':' '.' でない場合、1 桁で読んでも続かないので 2 桁を優先してよい。
                    i += 2;
                } else if (i < n && dec(s[i])) {
                    i += 1;
                } else {
                    groups = -1;
                    break;
                }
                if (groups >= 0) ++groups;
            }
            if (groups > 0 && i < n && s[i] == '.') {
                ++i;
                digits_us(i);
                if (i == n) return true;
            }
        }
    }
    // 4) [-+]?\.(?:inf|Inf|INF)
    {
        std::size_t i = 0;
        if (i < n && (s[i] == '-' || s[i] == '+')) ++i;
        const std::string rest = s.substr(i);
        if (rest == ".inf" || rest == ".Inf" || rest == ".INF") return true;
    }
    // 5) \.(?:nan|NaN|NAN)
    return s == ".nan" || s == ".NaN" || s == ".NAN";
}

bool resolve_float(const std::string& s) { return float_pattern(s) && !(!s.empty() && s.back() == '_'); }

bool resolve_timestamp(const std::string& s) {
    const std::size_t n = s.size();
    auto d = [&](std::size_t i) { return i < n && dec(s[i]); };
    // YAML_DATE_REGEXP: ^\d{4}-\d\d-\d\d$
    if (n == 10 && d(0) && d(1) && d(2) && d(3) && s[4] == '-' && d(5) && d(6) && s[7] == '-' && d(8) && d(9))
        return true;
    // YAML_TIMESTAMP_REGEXP
    std::size_t i = 0;
    if (!(d(0) && d(1) && d(2) && d(3))) return false;
    i = 4;
    auto one_or_two = [&](std::size_t& k) -> bool {
        if (!d(k)) return false;
        ++k;
        if (d(k)) ++k;
        return true;
    };
    if (i >= n || s[i] != '-') return false;
    ++i;
    if (!one_or_two(i)) return false;
    if (i >= n || s[i] != '-') return false;
    ++i;
    if (!one_or_two(i)) return false;
    if (i < n && (s[i] == 'T' || s[i] == 't')) {
        ++i;
    } else {
        const std::size_t start = i;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i == start) return false;
    }
    if (!one_or_two(i)) return false;
    if (i >= n || s[i] != ':') return false;
    ++i;
    if (!(d(i) && d(i + 1))) return false;
    i += 2;
    if (i >= n || s[i] != ':') return false;
    ++i;
    if (!(d(i) && d(i + 1))) return false;
    i += 2;
    if (i < n && s[i] == '.') {
        ++i;
        while (d(i)) ++i;
    }
    if (i == n) return true;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i < n && s[i] == 'Z') return i + 1 == n;
    if (i < n && (s[i] == '-' || s[i] == '+')) {
        ++i;
        if (!one_or_two(i)) return false;
        if (i == n) return true;
        if (s[i] != ':') return false;
        ++i;
        return d(i) && d(i + 1) && i + 2 == n;
    }
    return false;
}

bool resolve_merge(const std::string& s) { return s == "<<"; }

bool test_implicit_resolving(const std::string& s) {
    return resolve_null(s) || resolve_bool(s) || resolve_int(s) || resolve_float(s) || resolve_timestamp(s) ||
           resolve_merge(s);
}

// ─── スカラーの書式 ─────────────────────────────────────────────

enum class Style { plain, single, literal, folded, dbl };

std::string to_utf8(const Cps& cps, std::size_t begin = 0, std::size_t end = std::string::npos) {
    if (end > cps.size()) end = cps.size();
    std::string out;
    for (std::size_t i = begin; i < end; ++i) str::append_utf8(out, cps[i]);
    return out;
}

Style choose_scalar_style(const Cps& s, bool single_line_only, int line_width, const std::string& utf8) {
    bool has_line_break = false;
    bool has_foldable_line = false;
    const bool track_width = line_width != -1;
    long long previous_line_break = -1;
    const long long width = line_width;
    bool plain = is_plain_safe_first(s.front()) && !is_whitespace(s.back());
    std::size_t i = 0;
    auto char_after_break_is_space = [&](long long pb) {
        const auto k = static_cast<std::size_t>(pb + 1);
        return k < s.size() && s[k] == ' ';
    };
    if (single_line_only) {
        for (i = 0; i < s.size(); ++i) {
            const std::uint32_t c = s[i];
            if (!is_printable(c)) return Style::dbl;
            plain = plain && is_plain_safe(c, i > 0 ? s[i - 1] : 0);
        }
    } else {
        for (i = 0; i < s.size(); ++i) {
            const std::uint32_t c = s[i];
            if (c == 0x0A) {
                has_line_break = true;
                if (track_width) {
                    has_foldable_line = has_foldable_line ||
                                        (static_cast<long long>(i) - previous_line_break - 1 > width &&
                                         !char_after_break_is_space(previous_line_break));
                    previous_line_break = static_cast<long long>(i);
                }
            } else if (!is_printable(c)) {
                return Style::dbl;
            }
            plain = plain && is_plain_safe(c, i > 0 ? s[i - 1] : 0);
        }
        has_foldable_line = has_foldable_line ||
                            (track_width && (static_cast<long long>(i) - previous_line_break - 1 > width &&
                                             !char_after_break_is_space(previous_line_break)));
    }
    if (!has_line_break && !has_foldable_line) {
        return plain && !test_implicit_resolving(utf8) ? Style::plain : Style::single;
    }
    // indentPerLevel (2) <= 9 なので字下げ指示子の 1 桁制限には当たらない。
    return has_foldable_line ? Style::folded : Style::literal;
}

// /^\n* /
bool need_indent_indicator(const Cps& s) {
    std::size_t i = 0;
    while (i < s.size() && s[i] == '\n') ++i;
    return i < s.size() && s[i] == ' ';
}

std::string block_header(const Cps& s) {
    std::string out = need_indent_indicator(s) ? std::to_string(kIndent) : std::string();
    const bool clip = !s.empty() && s.back() == '\n';
    const bool keep = clip && ((s.size() >= 2 && s[s.size() - 2] == '\n') || s.size() == 1);
    out += keep ? "+" : (clip ? "" : "-");
    out += '\n';
    return out;
}

// 各行を字下げする (空行 "\n" だけの行は字下げしない)。
Cps indent_string(const Cps& s, int spaces) {
    Cps out;
    out.reserve(s.size() + s.size() / 8);
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t next = pos;
        while (next < s.size() && s[next] != '\n') ++next;
        const std::size_t end = next < s.size() ? next + 1 : s.size();
        const bool only_newline = end - pos == 1 && s[pos] == '\n';
        if (end > pos && !only_newline) out.insert(out.end(), static_cast<std::size_t>(spaces), ' ');
        out.insert(out.end(), s.begin() + static_cast<std::ptrdiff_t>(pos), s.begin() + static_cast<std::ptrdiff_t>(end));
        pos = end;
    }
    return out;
}

void drop_ending_newline(Cps& s) {
    if (!s.empty() && s.back() == '\n') s.pop_back();
}

// 貪欲な折り返し (dumper.js foldLine)。
Cps fold_line(const Cps& line, int width_i) {
    if (line.empty() || line[0] == ' ') return line;
    const std::size_t width = static_cast<std::size_t>(width_i);
    std::size_t start = 0, end = 0, curr = 0, next = 0;
    Cps result;
    auto append = [&](std::size_t a, std::size_t b) {
        result.insert(result.end(), line.begin() + static_cast<std::ptrdiff_t>(a),
                      line.begin() + static_cast<std::ptrdiff_t>(b));
    };
    // breakRe = / [^ ]/g (一致は重ならない)
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        if (line[i] != ' ' || line[i + 1] == ' ') continue;
        next = i;
        if (next - start > width) {
            end = curr > start ? curr : next;
            result.push_back('\n');
            append(start, end);
            start = end + 1;
        }
        curr = next;
        ++i;  // 一致は 2 文字を消費する
    }
    result.push_back('\n');
    if (line.size() - start > width && curr > start) {
        append(start, curr);
        result.push_back('\n');
        append(curr + 1, line.size());
    } else {
        append(start, line.size());
    }
    return Cps(result.begin() + 1, result.end());
}

// 折り返し (dumper.js foldString)。
Cps fold_string(const Cps& s, int width) {
    std::size_t next_lf = 0;
    while (next_lf < s.size() && s[next_lf] != '\n') ++next_lf;
    Cps result = fold_line(Cps(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(next_lf)), width);
    bool prev_more_indented = !s.empty() && (s[0] == '\n' || s[0] == ' ');
    // lineRe = /(\n+)([^\n]*)/g を next_lf から
    std::size_t i = next_lf;
    while (i < s.size()) {
        const std::size_t prefix_start = i;
        while (i < s.size() && s[i] == '\n') ++i;
        const std::size_t prefix_end = i;
        while (i < s.size() && s[i] != '\n') ++i;
        const Cps line(s.begin() + static_cast<std::ptrdiff_t>(prefix_end), s.begin() + static_cast<std::ptrdiff_t>(i));
        const bool more_indented = !line.empty() && line[0] == ' ';
        result.insert(result.end(), s.begin() + static_cast<std::ptrdiff_t>(prefix_start),
                      s.begin() + static_cast<std::ptrdiff_t>(prefix_end));
        if (!prev_more_indented && !more_indented && !line.empty()) result.push_back('\n');
        const Cps folded = fold_line(line, width);
        result.insert(result.end(), folded.begin(), folded.end());
        prev_more_indented = more_indented;
    }
    return result;
}

std::string encode_hex(std::uint32_t c) {
    char buf[16];
    if (c <= 0xFF) std::snprintf(buf, sizeof buf, "\\x%02X", static_cast<unsigned>(c));
    else if (c <= 0xFFFF) std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(c));
    else std::snprintf(buf, sizeof buf, "\\U%08X", static_cast<unsigned>(c));
    return buf;
}

const char* escape_sequence(std::uint32_t c) {
    switch (c) {
    case 0x00: return "\\0";
    case 0x07: return "\\a";
    case 0x08: return "\\b";
    case 0x09: return "\\t";
    case 0x0A: return "\\n";
    case 0x0B: return "\\v";
    case 0x0C: return "\\f";
    case 0x0D: return "\\r";
    case 0x1B: return "\\e";
    case 0x22: return "\\\"";
    case 0x5C: return "\\\\";
    case 0x85: return "\\N";
    case 0xA0: return "\\_";
    case 0x2028: return "\\L";
    case 0x2029: return "\\P";
    default: return nullptr;
    }
}

std::string escape_string(const Cps& s) {
    std::string out;
    for (const std::uint32_t c : s) {
        if (c >= 0x10000) {  // サロゲート対は結合して \U で書く
            out += encode_hex(c);
            continue;
        }
        const char* seq = escape_sequence(c);
        if (!seq && is_printable(c)) str::append_utf8(out, c);
        else out += seq ? std::string(seq) : encode_hex(c);
    }
    return out;
}

bool is_deprecated_boolean(const std::string& s) {
    static const char* const kWords[] = {"y", "Y", "yes", "Yes", "YES", "on", "On", "ON",
                                         "n", "N", "no", "No", "NO", "off", "Off", "OFF"};
    for (const char* w : kWords)
        if (s == w) return true;
    return false;
}

std::string write_scalar(const std::string& string, int level, bool iskey) {
    if (string.empty()) return "''";
    if (is_deprecated_boolean(string)) return "'" + string + "'";
    const int indent = kIndent * std::max(1, level);
    const int line_width = std::max(std::min(kLineWidth, 40), kLineWidth - indent);
    const Cps cps = str::codepoints(string);
    switch (choose_scalar_style(cps, iskey, line_width, string)) {
    case Style::plain:
        return string;
    case Style::single:
        return "'" + str::replace_all(string, "'", "''") + "'";
    case Style::literal: {
        Cps body = indent_string(cps, indent);
        drop_ending_newline(body);
        return "|" + block_header(cps) + to_utf8(body);
    }
    case Style::folded: {
        Cps body = indent_string(fold_string(cps, line_width), indent);
        drop_ending_newline(body);
        return ">" + block_header(cps) + to_utf8(body);
    }
    case Style::dbl:
        return "\"" + escape_string(cps) + "\"";
    }
    throw std::logic_error("front_matter: invalid scalar style");
}

// ─── 数値 (int / float の represent) ────────────────────────────────

std::string represent_number(const Json& v) {
    double d;
    if (v.is_number_integer()) d = v.is_number_unsigned() ? static_cast<double>(v.get<std::uint64_t>())
                                                           : static_cast<double>(v.get<std::int64_t>());
    else d = v.get<double>();
    if (std::isnan(d)) return ".nan";
    if (std::isinf(d)) return d > 0 ? ".inf" : "-.inf";
    const bool negative_zero = d == 0 && std::signbit(d);
    if (std::fmod(d, 1.0) == 0 && !negative_zero) return js_number_to_string(d);  // int (decimal)
    if (negative_zero) return "-0.0";
    std::string res = js_number_to_string(d);
    // /^[-+]?[0-9]+e/ → 'e' を '.e' に (YAML は指数表記に小数点が要る)
    std::size_t i = 0;
    if (i < res.size() && (res[i] == '-' || res[i] == '+')) ++i;
    const std::size_t digits_start = i;
    while (i < res.size() && dec(res[i])) ++i;
    if (i > digits_start && i < res.size() && res[i] == 'e') res.replace(i, 1, ".e");
    return res;
}

// ─── キーの順序 (JS の Object.keys: 配列添字のキーが昇順で先、残りは挿入順) ─────

bool is_array_index_key(const std::string& k, std::uint32_t* out) {
    if (k.empty() || k.size() > 10) return false;
    if (k.size() > 1 && k[0] == '0') return false;
    std::uint64_t v = 0;
    for (char c : k) {
        if (!dec(c)) return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (v > 4294967294ULL) return false;  // 2^32 - 2 まで
    *out = static_cast<std::uint32_t>(v);
    return true;
}

std::vector<std::pair<std::string, const Json*>> js_ordered_entries(const Json& object) {
    std::vector<std::pair<std::uint32_t, std::pair<std::string, const Json*>>> indexed;
    std::vector<std::pair<std::string, const Json*>> named;
    for (auto it = object.begin(); it != object.end(); ++it) {
        std::uint32_t idx = 0;
        if (is_array_index_key(it.key(), &idx)) indexed.push_back({idx, {it.key(), &it.value()}});
        else named.push_back({it.key(), &it.value()});
    }
    std::sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<std::string, const Json*>> out;
    out.reserve(indexed.size() + named.size());
    for (auto& e : indexed) out.push_back(std::move(e.second));
    for (auto& e : named) out.push_back(std::move(e));
    return out;
}

// ─── ノード ────────────────────────────────────────────────────

std::string next_line(int level) { return "\n" + std::string(static_cast<std::size_t>(kIndent * level), ' '); }

std::string write_node(const Json& v, int level, bool compact, std::size_t depth);

std::string write_block_mapping(const Json& object, int level, bool compact, std::size_t depth) {
    std::string result;
    std::size_t index = 0;
    for (const auto& [key, value] : js_ordered_entries(object)) {
        std::string pair;
        if (!compact || index != 0) pair += next_line(level);
        ++index;
        const std::string key_dump = write_scalar(key, level + 1, true);
        const bool explicit_pair = str::js_length(key_dump) > 1024;
        if (explicit_pair) pair += (!key_dump.empty() && key_dump[0] == '\n') ? "?" : "? ";
        pair += key_dump;
        if (explicit_pair) pair += next_line(level);
        const std::string value_dump = write_node(*value, level + 1, explicit_pair, depth + 1);
        pair += (!value_dump.empty() && value_dump[0] == '\n') ? ":" : ": ";
        pair += value_dump;
        result += pair;
    }
    return result.empty() ? "{}" : result;
}

std::string write_block_sequence(const Json& array, int level, bool compact, std::size_t depth) {
    std::string result;
    std::size_t index = 0;
    for (const auto& item : array) {
        const std::string dump = write_node(item, level + 1, true, depth + 1);
        if (!compact || index != 0) result += next_line(level);
        ++index;
        result += (!dump.empty() && dump[0] == '\n') ? "-" : "- ";
        result += dump;
    }
    return result.empty() ? "[]" : result;
}

std::string write_node(const Json& v, int level, bool compact, std::size_t depth) {
    if (depth > kMaxDepth) throw std::invalid_argument("front_matter: data is nested too deeply");
    switch (v.type()) {
    case Json::value_t::null:
        return "null";
    case Json::value_t::boolean:
        return v.get<bool>() ? "true" : "false";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
    case Json::value_t::number_float:
        return represent_number(v);
    case Json::value_t::string:
        return write_scalar(v.get_ref<const std::string&>(), level, false);
    case Json::value_t::object:
        // block (flowLevel=-1) で中身があればブロック、空なら flow の "{}"
        return v.empty() ? "{}" : write_block_mapping(v, level, compact, depth);
    case Json::value_t::array:
        return v.empty() ? "[]" : write_block_sequence(v, level, compact, depth);
    default:
        // js-yaml は undefined / 関数 等を "unacceptable kind of an object to dump" で拒否する
        throw std::invalid_argument("front_matter: unacceptable kind of an object to dump");
    }
}

// Object.assign({}, x) で x の自分のプロパティを target に写す (JSON で表せる範囲)。
void assign_into(Json& target, const Json& source) {
    if (source.is_object()) {
        for (auto it = source.begin(); it != source.end(); ++it) target[it.key()] = it.value();
    } else if (source.is_array()) {
        for (std::size_t i = 0; i < source.size(); ++i) target[std::to_string(i)] = source[i];
    } else if (source.is_string()) {
        // 文字列は UTF-16 の 1 単位ずつ添字のキーになる。BMP 外の文字はサロゲートの片割れになり
        // UTF-8 で表せないので拒否する。
        std::size_t i = 0;
        for (const std::uint32_t c : str::codepoints(source.get_ref<const std::string&>())) {
            if (c >= 0x10000) throw std::invalid_argument("front_matter: string data with non-BMP characters");
            std::string one;
            str::append_utf8(one, c);
            target[std::to_string(i++)] = one;
        }
    }
    // null / 数値 / 真偽値は何も写さない
}

std::string newline(std::string s) {
    if (s.empty() || s.back() != '\n') s += '\n';
    return s;
}

} // namespace

std::string dump_yaml(const Json& data) { return write_node(data, 0, true, 0) + "\n"; }

std::string stringify(std::string_view content, const Json& data) {
    // matter.stringify(string, data) は先に matter(string) で読み、既存の front matter と data を合わせる。
    std::string error;
    auto file = parse(content, &error);
    if (!file) throw std::runtime_error("front_matter: " + error);
    Json merged = Json::object();
    assign_into(merged, file->data);
    // data が null なら file.data をそのまま使う (stringify.js の data == null 分岐)
    if (!data.is_null()) assign_into(merged, data);
    const std::string matter = str::trim(dump_yaml(merged));
    std::string buf;
    if (matter != "{}") buf = newline("---") + newline(matter) + newline("---");
    return buf + newline(file->content);
}

} // namespace di::lib::front_matter
