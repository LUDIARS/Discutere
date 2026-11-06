// front matter の読み (YAML の部分集合)。書き (dump) は front_matter_dump.cpp。
#include "lib/front_matter/front_matter.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

#include <lorica/input_budget.hpp>

#include "base/strings.hpp"
#include "base/time.hpp"

namespace di::lib::front_matter {
namespace {

struct YamlError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Line {
    int indent;        // 先頭の空白数
    std::string text;  // 字下げを除いた本文 (行末の空白は残す)
    int number;        // 1 始まり (エラー表示用)
    bool blank;        // 空行・コメントだけの行
};

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// ─── スカラーの型解決 (js-yaml 3 の safe schema) ─────────────────────

bool resolve_null(const std::string& s) { return s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL"; }

std::optional<bool> resolve_bool(const std::string& s) {
    if (s == "true" || s == "True" || s == "TRUE") return true;
    if (s == "false" || s == "False" || s == "FALSE") return false;
    return std::nullopt;
}

std::optional<Json> resolve_int(const std::string& data) {
    if (data.empty()) return std::nullopt;
    std::size_t i = 0;
    int sign = 1;
    if (data[i] == '-' || data[i] == '+') {
        if (data[i] == '-') sign = -1;
        ++i;
    }
    if (i >= data.size()) return std::nullopt;
    auto parse_base = [&](std::size_t start, int base) -> std::optional<Json> {
        if (start >= data.size() || data.back() == '_') return std::nullopt;
        double value = 0;
        bool digits = false;
        for (std::size_t k = start; k < data.size(); ++k) {
            const char c = data[k];
            if (c == '_') continue;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return std::nullopt;
            if (d >= base) return std::nullopt;
            value = value * base + d;
            digits = true;
        }
        if (!digits) return std::nullopt;
        value *= sign;
        if (std::fabs(value) < 9.0e15) return Json(static_cast<std::int64_t>(value));
        return Json(value);
    };
    if (data[i] == '0') {
        if (i + 1 == data.size()) return Json(static_cast<std::int64_t>(0));
        const char next = data[i + 1];
        if (next == 'b') return parse_base(i + 2, 2);
        if (next == 'x') return parse_base(i + 2, 16);
        return parse_base(i + 1, 8);
    }
    // 10 進 (先頭以外の _ は許す。60 進数 1:20 は拒否側で扱う)
    if (!is_digit(data[i])) return std::nullopt;
    for (std::size_t k = i; k < data.size(); ++k)
        if (!is_digit(data[k]) && data[k] != '_') return std::nullopt;
    return parse_base(i, 10);
}

std::optional<Json> resolve_float(const std::string& data) {
    if (data.empty() || data.back() == '_') return std::nullopt;
    const std::string lower = str::to_lower_ascii(data);
    if (lower == ".inf" || lower == "+.inf") return Json(std::numeric_limits<double>::infinity());
    if (lower == "-.inf") return Json(-std::numeric_limits<double>::infinity());
    if (lower == ".nan") return Json(std::numeric_limits<double>::quiet_NaN());
    // ^[-+]?(?:0|[1-9][0-9_]*)(?:\.[0-9_]*)?(?:[eE][-+]?[0-9]+)?$  |  ^\.[0-9_]+(?:[eE][-+]?[0-9]+)?$
    std::size_t i = 0;
    std::string digits;
    if (data[i] == '-' || data[i] == '+') {
        if (data[i] == '-') digits.push_back('-');
        ++i;
    }
    bool int_part = false;
    if (i < data.size() && data[i] == '0') {
        digits.push_back('0');
        ++i;
        int_part = true;
    } else if (i < data.size() && data[i] >= '1' && data[i] <= '9') {
        while (i < data.size() && (is_digit(data[i]) || data[i] == '_')) {
            if (data[i] != '_') digits.push_back(data[i]);
            ++i;
        }
        int_part = true;
    }
    bool frac = false;
    if (i < data.size() && data[i] == '.') {
        digits.push_back('.');
        ++i;
        while (i < data.size() && (is_digit(data[i]) || data[i] == '_')) {
            if (data[i] != '_') {
                digits.push_back(data[i]);
                frac = true;
            }
            ++i;
        }
        if (!int_part && !frac) return std::nullopt;
    } else if (!int_part) {
        return std::nullopt;
    }
    if (i < data.size() && (data[i] == 'e' || data[i] == 'E')) {
        digits.push_back('e');
        ++i;
        if (i < data.size() && (data[i] == '-' || data[i] == '+')) digits.push_back(data[i++]);
        std::size_t start = i;
        while (i < data.size() && is_digit(data[i])) digits.push_back(data[i++]);
        if (i == start) return std::nullopt;
    }
    if (i != data.size()) return std::nullopt;
    if (!digits.empty() && digits.back() == '.') digits.pop_back();
    auto v = str::parse_double(digits);
    if (!v) return std::nullopt;
    return Json(*v);
}

std::optional<Json> resolve_timestamp(const std::string& s) {
    // ^YYYY-MM-DD$ または YYYY-M(M)-D(D)[Tt ]H(H):MM:SS(.f)?(Z|±H(H)(:MM)?)?
    if (s.size() < 8 || !is_digit(s[0]) || !is_digit(s[1]) || !is_digit(s[2]) || !is_digit(s[3]) || s[4] != '-') return std::nullopt;
    std::size_t i = 5;
    auto read_num = [&](std::size_t min_digits, std::size_t max_digits, int& out) {
        std::size_t start = i;
        int v = 0;
        while (i < s.size() && is_digit(s[i]) && i - start < max_digits) v = v * 10 + (s[i++] - '0');
        if (i - start < min_digits) return false;
        out = v;
        return true;
    };
    const int year = std::stoi(s.substr(0, 4));
    int month, day;
    if (!read_num(1, 2, month) || i >= s.size() || s[i++] != '-' || !read_num(1, 2, day)) return std::nullopt;
    if (i == s.size()) {
        if (s.size() != 10) return std::nullopt;  // 日付だけの形は 2 桁ずつに限る
        return Json(time::iso_utc(time::from_civil_utc(year, month, day)));
    }
    if (s[i] == 'T' || s[i] == 't') ++i;
    else if (s[i] == ' ' || s[i] == '\t') {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    } else return std::nullopt;
    int hour, minute, second;
    if (!read_num(1, 2, hour) || i >= s.size() || s[i++] != ':' || !read_num(2, 2, minute) || i >= s.size() || s[i++] != ':' ||
        !read_num(2, 2, second))
        return std::nullopt;
    int millis = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        std::string frac;
        while (i < s.size() && is_digit(s[i])) frac.push_back(s[i++]);
        frac = (frac + "000").substr(0, 3);
        millis = std::stoi(frac);
    }
    std::int64_t offset = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i < s.size()) {
        if (s[i] == 'Z') {
            ++i;
        } else if (s[i] == '+' || s[i] == '-') {
            const int sign = s[i++] == '-' ? -1 : 1;
            int th, tm = 0;
            if (!read_num(1, 2, th)) return std::nullopt;
            if (i < s.size() && s[i] == ':') {
                ++i;
                if (!read_num(2, 2, tm)) return std::nullopt;
            }
            offset = sign * (static_cast<std::int64_t>(th) * 60 + tm) * 60000;
        } else {
            return std::nullopt;
        }
    }
    if (i != s.size()) return std::nullopt;
    return Json(time::iso_utc(time::from_civil_utc(year, month, day, hour, minute, second, millis) - offset));
}

Json resolve_plain(const std::string& s) {
    if (resolve_null(s)) return nullptr;
    if (auto b = resolve_bool(s)) return *b;
    if (auto i = resolve_int(s)) return *i;
    if (auto f = resolve_float(s)) return *f;
    if (auto t = resolve_timestamp(s)) return *t;
    return s;
}

// ─── パーサ ──────────────────────────────────────────────────────

class Parser {
public:
    Parser(std::vector<Line> lines, const Limits& limits)
        : lines_(std::move(lines)), budget_({limits.max_bytes, limits.max_elements, limits.max_elements, limits.max_depth}) {}

    Json parse_document() {
        skip_blank();
        if (pos_ >= lines_.size()) return nullptr;
        Json v = parse_node(-1, 0);
        skip_blank();
        if (pos_ < lines_.size()) fail("unexpected content");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) {
        const int line = pos_ < lines_.size() ? lines_[pos_].number : (lines_.empty() ? 0 : lines_.back().number);
        throw YamlError("yaml: " + what + " (line " + std::to_string(line) + ")");
    }

    void charge(std::size_t depth) {
        if (!budget_.consume(0, 1, 1)) fail("too many elements");
        if (!budget_.permits_depth(depth)) fail("nesting too deep");
    }

    void skip_blank() {
        while (pos_ < lines_.size() && lines_[pos_].blank) ++pos_;
    }

    static bool is_seq_entry(const std::string& t) { return t == "-" || str::starts_with(t, "- ") || str::starts_with(t, "-\t"); }

    /** 引用の外の "key: " / "key:" (行末) の ':' の位置。無ければ npos。 */
    static std::size_t find_mapping_colon(const std::string& t) {
        if (t.empty()) return std::string::npos;
        std::size_t i = 0;
        if (t[0] == '"' || t[0] == '\'') {
            const char q = t[0];
            i = 1;
            while (i < t.size()) {
                if (q == '\'' && t[i] == '\'' && i + 1 < t.size() && t[i + 1] == '\'') {
                    i += 2;
                    continue;
                }
                if (q == '"' && t[i] == '\\') {
                    i += 2;
                    continue;
                }
                if (t[i] == q) break;
                ++i;
            }
            if (i >= t.size()) return std::string::npos;
            ++i;
            while (i < t.size() && t[i] == ' ') ++i;
            if (i < t.size() && t[i] == ':' && (i + 1 == t.size() || t[i + 1] == ' ' || t[i + 1] == '\t')) return i;
            return std::string::npos;
        }
        if (t[0] == '[' || t[0] == '{' || t[0] == '#') return std::string::npos;
        for (i = 0; i < t.size(); ++i) {
            if (t[i] == '#' && i > 0 && (t[i - 1] == ' ' || t[i - 1] == '\t')) return std::string::npos;
            if (t[i] == ':' && (i + 1 == t.size() || t[i + 1] == ' ' || t[i + 1] == '\t')) return i;
        }
        return std::string::npos;
    }

    /** 行末コメントを除く (引用の外の " #")。 */
    static std::string strip_comment(const std::string& t) {
        bool in_single = false, in_double = false;
        for (std::size_t i = 0; i < t.size(); ++i) {
            const char c = t[i];
            if (in_double) {
                if (c == '\\') ++i;
                else if (c == '"') in_double = false;
                continue;
            }
            if (in_single) {
                if (c == '\'') in_single = false;
                continue;
            }
            if (c == '"' && (i == 0 || t[i - 1] == ' ' || t[i - 1] == '[' || t[i - 1] == '{' || t[i - 1] == ',')) in_double = true;
            else if (c == '\'' && (i == 0 || t[i - 1] == ' ' || t[i - 1] == '[' || t[i - 1] == '{' || t[i - 1] == ',')) in_single = true;
            else if (c == '#' && (i == 0 || t[i - 1] == ' ' || t[i - 1] == '\t')) return str::trim_end(t.substr(0, i));
        }
        return str::trim_end(t);
    }

    Json parse_node(int parent_indent, std::size_t depth) {
        skip_blank();
        if (pos_ >= lines_.size()) return nullptr;
        const Line& line = lines_[pos_];
        if (line.indent <= parent_indent) return nullptr;
        if (is_seq_entry(line.text)) return parse_sequence(line.indent, depth);
        if (find_mapping_colon(line.text) != std::string::npos) return parse_mapping(line.indent, depth);
        // スカラーだけの値
        const int indent = line.indent;
        std::string text = line.text;
        ++pos_;
        return parse_inline_value(text, parent_indent < 0 ? -1 : parent_indent, indent, depth);
    }

    Json parse_sequence(int indent, std::size_t depth) {
        charge(depth + 1);
        Json arr = Json::array();
        while (true) {
            skip_blank();
            if (pos_ >= lines_.size()) break;
            Line& line = lines_[pos_];
            if (line.indent != indent || !is_seq_entry(line.text)) {
                if (line.indent > indent) fail("bad indentation of a sequence entry");
                break;
            }
            charge(depth + 1);
            std::string rest = line.text.size() > 1 ? line.text.substr(1) : std::string();
            std::size_t spaces = 0;
            while (spaces < rest.size() && (rest[spaces] == ' ' || rest[spaces] == '\t')) ++spaces;
            rest = rest.substr(spaces);
            if (rest.empty() || rest[0] == '#') {
                ++pos_;
                skip_blank();
                if (pos_ < lines_.size() && lines_[pos_].indent > indent) arr.push_back(parse_node(indent, depth + 1));
                else arr.push_back(nullptr);
                continue;
            }
            // "- key: v" / "- - x" は、同じ行を字下げの深い仮の行として読み直す。
            line.indent = indent + 1 + static_cast<int>(spaces);
            line.text = rest;
            if (is_seq_entry(rest) || find_mapping_colon(rest) != std::string::npos) {
                arr.push_back(parse_node(indent, depth + 1));
            } else {
                ++pos_;
                arr.push_back(parse_inline_value(rest, indent, line.indent, depth + 1));
            }
        }
        return arr;
    }

    std::string parse_key(const std::string& raw) {
        const std::string k = str::trim(raw);
        if (k.empty()) fail("empty mapping key");
        if (k[0] == '?' || k[0] == '&' || k[0] == '*' || k[0] == '!' || k[0] == '[' || k[0] == '{')
            fail("unsupported mapping key");
        if (k[0] == '"' || k[0] == '\'') {
            std::size_t i = 0;
            std::string out = parse_quoted(k, i);
            if (str::trim(k.substr(i)).size() != 0) fail("bad quoted key");
            return out;
        }
        // js-yaml はキーを文字列にする (数値のキーも "1")。null のキーは "null"。
        return k;
    }

    Json parse_mapping(int indent, std::size_t depth) {
        charge(depth + 1);
        Json obj = Json::object();
        while (true) {
            skip_blank();
            if (pos_ >= lines_.size()) break;
            const Line line = lines_[pos_];
            if (line.indent != indent) {
                if (line.indent > indent) fail("bad indentation of a mapping entry");
                break;
            }
            if (is_seq_entry(line.text)) break;
            const auto colon = find_mapping_colon(line.text);
            if (colon == std::string::npos) fail("expected a mapping entry");
            charge(depth + 1);
            const std::string key = parse_key(line.text.substr(0, colon));
            if (obj.contains(key)) fail("duplicated mapping key");
            std::string rest = str::trim(line.text.substr(colon + 1));
            ++pos_;
            Json value;
            if (rest.empty() || rest[0] == '#') {
                skip_blank();
                if (pos_ < lines_.size() && lines_[pos_].indent > indent) value = parse_node(indent, depth + 1);
                else if (pos_ < lines_.size() && lines_[pos_].indent == indent && is_seq_entry(lines_[pos_].text))
                    value = parse_sequence(indent, depth + 1);
                else value = nullptr;
            } else {
                value = parse_inline_value(rest, indent, indent, depth + 1);
            }
            obj[key] = std::move(value);
        }
        return obj;
    }

    /** 行の残り (rest) から値を読む。必要なら後続の行 (parent_indent より深い) も使う。 */
    Json parse_inline_value(const std::string& raw, int parent_indent, int own_indent, std::size_t depth) {
        charge(depth + 1);
        std::string rest = raw;
        if (rest.empty()) return nullptr;
        const char c = rest[0];
        if (c == '&' || c == '*' || c == '!' || c == '%' || c == '@' || c == '`') fail("unsupported YAML feature");
        if (c == '|' || c == '>') return parse_block_scalar(rest, parent_indent);
        if (c == '"' || c == '\'') {
            std::string acc = rest;
            // 閉じていなければ後続行を折り返しとして足す。
            while (true) {
                std::size_t i = 0;
                try {
                    std::string value = parse_quoted(acc, i);
                    const std::string tail = strip_comment(acc.substr(i));
                    if (!str::trim(tail).empty()) fail("unexpected text after quoted scalar");
                    return value;
                } catch (const YamlError& e) {
                    if (std::string(e.what()).find("unterminated") == std::string::npos) throw;
                }
                if (pos_ >= lines_.size() || (!lines_[pos_].blank && lines_[pos_].indent <= parent_indent)) fail("unterminated quoted scalar");
                const Line& next = lines_[pos_++];
                acc += next.blank ? std::string("\n") : "\n" + next.text;
            }
        }
        if (c == '[' || c == '{') {
            std::string acc = strip_comment(rest);
            // 1 行に収まらないフロー表記は後続行 (より深い字下げ) を繋げる。
            int balance = 0;
            auto unbalanced = [&](const std::string& s) {
                balance = 0;
                bool sq = false, dq = false;
                for (std::size_t i = 0; i < s.size(); ++i) {
                    const char ch = s[i];
                    if (dq) { if (ch == '\\') ++i; else if (ch == '"') dq = false; continue; }
                    if (sq) { if (ch == '\'') sq = false; continue; }
                    if (ch == '"') dq = true;
                    else if (ch == '\'') sq = true;
                    else if (ch == '[' || ch == '{') ++balance;
                    else if (ch == ']' || ch == '}') --balance;
                }
                return balance > 0;
            };
            while (unbalanced(acc)) {
                if (pos_ >= lines_.size() || lines_[pos_].indent <= parent_indent) fail("unterminated flow collection");
                const Line& next = lines_[pos_++];
                if (!next.blank) acc += " " + strip_comment(next.text);
            }
            std::size_t i = 0;
            Json v = parse_flow(acc, i, depth + 1);
            skip_spaces(acc, i);
            if (i != acc.size()) fail("unexpected text after flow collection");
            return v;
        }
        // 平文スカラー: 後続のより深い行は空白で折り返して繋げる (空行は改行)。コメント行で終わる。
        std::string folded = strip_comment(rest);
        (void)own_indent;
        while (pos_ < lines_.size()) {
            const Line& next = lines_[pos_];
            if (next.blank) {
                if (!str::trim(next.text).empty()) break;  // コメント行
                std::size_t look = pos_;
                std::size_t empties = 0;
                while (look < lines_.size() && lines_[look].blank && str::trim(lines_[look].text).empty()) {
                    ++look;
                    ++empties;
                }
                if (look >= lines_.size() || lines_[look].blank || lines_[look].indent <= parent_indent) break;
                if (find_mapping_colon(lines_[look].text) != std::string::npos) break;
                folded += std::string(empties, '\n');
                folded += strip_comment(lines_[look].text);
                pos_ = look + 1;
                continue;
            }
            if (next.indent <= parent_indent) break;
            if (find_mapping_colon(next.text) != std::string::npos) fail("mapping values are not allowed in this context");
            folded += " ";
            folded += strip_comment(next.text);
            ++pos_;
        }
        return resolve_plain(str::trim(folded));
    }

    Json parse_block_scalar(const std::string& header, int parent_indent) {
        const bool literal = header[0] == '|';
        char chomp = 'c';
        int explicit_indent = 0;
        for (std::size_t i = 1; i < header.size(); ++i) {
            const char ch = header[i];
            if (ch == '+' || ch == '-') chomp = ch;
            else if (ch >= '1' && ch <= '9') explicit_indent = ch - '0';
            else if (ch == ' ' || ch == '\t') {
                const std::string tail = str::trim(header.substr(i));
                if (!tail.empty() && tail[0] != '#') fail("bad block scalar header");
                break;
            } else fail("bad block scalar header");
        }
        const int base = std::max(parent_indent, 0);
        int content_indent = explicit_indent ? base + explicit_indent : -1;
        if (parent_indent < 0 && explicit_indent) content_indent = explicit_indent;
        std::vector<std::string> body;
        std::size_t trailing_blank = 0;
        while (pos_ < lines_.size()) {
            const Line& l = lines_[pos_];
            const bool empty = str::trim(l.text).empty();
            if (empty) {
                body.emplace_back();
                ++trailing_blank;
                ++pos_;
                continue;
            }
            if (content_indent < 0) {
                if (l.indent <= parent_indent) break;
                content_indent = l.indent;
            }
            if (l.indent < content_indent) break;
            body.push_back(std::string(static_cast<std::size_t>(l.indent - content_indent), ' ') + l.text);
            trailing_blank = 0;
            ++pos_;
        }
        // 末尾の空行は本文から外し、chomping で扱う。
        // (空行の後にブロックが終わった場合、その空行はブロック外の行でもありうるが、YAML ではブロック側に属する)
        std::size_t content_lines = body.size() - trailing_blank;
        std::string out;
        bool prev_more_indented = false;
        bool prev_blank = false;
        for (std::size_t i = 0; i < content_lines; ++i) {
            const std::string& l = body[i];
            if (literal) {
                if (i) out.push_back('\n');
                out += l;
                continue;
            }
            const bool blank = l.empty();
            const bool more = !blank && (l[0] == ' ' || l[0] == '\t');
            if (i == 0) {
                out += l;
            } else if (blank) {
                out.push_back('\n');
            } else if (prev_blank) {
                if (more || prev_more_indented) out.push_back('\n');
                out += l;
            } else if (more || prev_more_indented) {
                out.push_back('\n');
                out += l;
            } else {
                out.push_back(' ');
                out += l;
            }
            if (!blank) prev_more_indented = more;
            prev_blank = blank;
        }
        if (chomp == '-') return out;
        if (content_lines == 0) return chomp == '+' ? std::string(trailing_blank, '\n') : std::string();
        out.push_back('\n');
        if (chomp == '+') out.append(trailing_blank, '\n');
        return out;
    }

    static void skip_spaces(const std::string& s, std::size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    }

    std::string parse_quoted(const std::string& s, std::size_t& i) {
        const char q = s[i++];
        std::string out;
        while (true) {
            if (i >= s.size()) fail("unterminated quoted scalar");
            const char c = s[i];
            if (q == '\'') {
                if (c == '\'') {
                    if (i + 1 < s.size() && s[i + 1] == '\'') {
                        out.push_back('\'');
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
            } else {
                if (c == '"') {
                    ++i;
                    break;
                }
                if (c == '\\') {
                    if (i + 1 >= s.size()) fail("unterminated quoted scalar");
                    const char e = s[i + 1];
                    i += 2;
                    auto hex = [&](std::size_t n) {
                        if (i + n > s.size()) fail("bad escape");
                        std::uint32_t cp = 0;
                        for (std::size_t k = 0; k < n; ++k) {
                            const char h = s[i + k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
                            else fail("bad escape");
                        }
                        i += n;
                        str::append_utf8(out, cp);
                    };
                    switch (e) {
                    case '0': out.push_back('\0'); break;
                    case 'a': out.push_back('\a'); break;
                    case 'b': out.push_back('\b'); break;
                    case 't': case '\t': out.push_back('\t'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'v': out.push_back('\v'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'e': out.push_back('\x1b'); break;
                    case ' ': out.push_back(' '); break;
                    case '"': out.push_back('"'); break;
                    case '/': out.push_back('/'); break;
                    case '\\': out.push_back('\\'); break;
                    case 'N': str::append_utf8(out, 0x85); break;
                    case '_': str::append_utf8(out, 0xA0); break;
                    case 'L': str::append_utf8(out, 0x2028); break;
                    case 'P': str::append_utf8(out, 0x2029); break;
                    case 'x': hex(2); break;
                    case 'u': hex(4); break;
                    case 'U': hex(8); break;
                    case '\n':
                        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
                        break;
                    default: fail("bad escape");
                    }
                    continue;
                }
            }
            if (c == '\n') {
                // 折り返し: 改行 1 つは空白、連続する改行は (数-1) 個の改行。前後の空白は捨てる。
                while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
                std::size_t newlines = 0;
                while (i < s.size() && (s[i] == '\n' || s[i] == ' ' || s[i] == '\t')) {
                    if (s[i] == '\n') ++newlines;
                    ++i;
                }
                if (newlines <= 1) out.push_back(' ');
                else out.append(newlines - 1, '\n');
                continue;
            }
            out.push_back(c);
            ++i;
        }
        return out;
    }

    Json parse_flow(const std::string& s, std::size_t& i, std::size_t depth) {
        charge(depth);
        skip_spaces(s, i);
        if (i >= s.size()) fail("unexpected end of flow collection");
        const char open = s[i];
        if (open == '[' || open == '{') {
            const char close = open == '[' ? ']' : '}';
            ++i;
            Json out = open == '[' ? Json::array() : Json::object();
            skip_spaces(s, i);
            if (i < s.size() && s[i] == close) {
                ++i;
                return out;
            }
            while (true) {
                skip_spaces(s, i);
                if (open == '[') {
                    out.push_back(parse_flow(s, i, depth + 1));
                } else {
                    std::string key;
                    if (i < s.size() && (s[i] == '"' || s[i] == '\'')) key = parse_quoted(s, i);
                    else {
                        const std::size_t start = i;
                        while (i < s.size() && s[i] != ':' && s[i] != ',' && s[i] != '}') ++i;
                        key = str::trim(s.substr(start, i - start));
                    }
                    skip_spaces(s, i);
                    Json value = nullptr;
                    if (i < s.size() && s[i] == ':') {
                        ++i;
                        skip_spaces(s, i);
                        if (i < s.size() && s[i] != ',' && s[i] != '}') value = parse_flow(s, i, depth + 1);
                    }
                    if (out.contains(key)) fail("duplicated mapping key");
                    out[key] = std::move(value);
                }
                skip_spaces(s, i);
                if (i >= s.size()) fail("unterminated flow collection");
                if (s[i] == ',') {
                    ++i;
                    skip_spaces(s, i);
                    if (i < s.size() && s[i] == close) {
                        ++i;
                        return out;
                    }
                    continue;
                }
                if (s[i] == close) {
                    ++i;
                    return out;
                }
                fail("bad flow collection");
            }
        }
        if (open == '"' || open == '\'') return parse_quoted(s, i);
        if (open == '&' || open == '*' || open == '!' || open == '?') fail("unsupported YAML feature");
        const std::size_t start = i;
        while (i < s.size() && s[i] != ',' && s[i] != ']' && s[i] != '}') {
            if (s[i] == ':' && (i + 1 >= s.size() || s[i + 1] == ' ')) break;
            ++i;
        }
        return resolve_plain(str::trim(s.substr(start, i - start)));
    }

    std::vector<Line> lines_;
    std::size_t pos_ = 0;
    lorica::input_budget budget_;
};

std::vector<Line> split_lines(std::string_view yaml) {
    std::vector<Line> lines;
    int number = 0;
    auto raw_lines = str::split_lines(yaml);
    // 末尾の改行の後ろは行ではない (str::split_lines は空の要素を返す)。
    if (!raw_lines.empty() && raw_lines.back().empty()) raw_lines.pop_back();
    for (const auto& raw : raw_lines) {
        ++number;
        int indent = 0;
        while (static_cast<std::size_t>(indent) < raw.size() && raw[static_cast<std::size_t>(indent)] == ' ') ++indent;
        std::string text = raw.substr(static_cast<std::size_t>(indent));
        if (!text.empty() && text[0] == '\t' && !str::trim(text).empty()) {
            if (str::trim(text)[0] != '#') throw YamlError("yaml: tab characters must not be used in indentation (line " + std::to_string(number) + ")");
        }
        const std::string trimmed = str::trim(text);
        const bool blank = trimmed.empty() || trimmed[0] == '#';
        if (!blank && indent == 0 && (str::starts_with(trimmed, "---") || str::starts_with(trimmed, "...") || trimmed[0] == '%'))
            throw YamlError("yaml: multiple documents or directives are not supported (line " + std::to_string(number) + ")");
        lines.push_back({indent, str::trim_end(text), number, blank});
    }
    return lines;
}

} // namespace

std::optional<Json> parse_yaml(std::string_view yaml, std::string* error, const Limits& limits) {
    if (yaml.size() > limits.max_bytes) {
        if (error) *error = "yaml: front matter too large";
        return std::nullopt;
    }
    try {
        Parser parser(split_lines(yaml), limits);
        return parser.parse_document();
    } catch (const YamlError& e) {
        if (error) *error = e.what();
    } catch (const std::exception& e) {
        if (error) *error = std::string("yaml: ") + e.what();
    }
    return std::nullopt;
}

std::optional<Matter> parse(std::string_view input, std::string* error, const Limits& limits) {
    Matter m;
    if (input.empty()) return m;
    std::string text(input);
    if (str::starts_with(text, "\xEF\xBB\xBF")) text.erase(0, 3);  // BOM
    m.content = text;
    if (!str::starts_with(text, "---")) return m;
    if (text.size() > 3 && text[3] == '-') return m;
    std::string str_ = text.substr(3);
    // "---yaml" のような言語指定は yaml のみ受ける。
    const auto first_nl = str_.find('\n');
    const std::string lang = str::trim(str_.substr(0, first_nl == std::string::npos ? str_.size() : first_nl));
    if (!lang.empty()) {
        if (lang != "yaml" && lang != "yml") {
            if (error) *error = "front matter language not supported: " + lang;
            return std::nullopt;
        }
        str_ = str_.substr(str_.find(lang) + lang.size());
    }
    auto close = str_.find("\n---");
    const bool closed = close != std::string::npos;
    if (!closed) close = str_.size();
    const std::string block = str_.substr(0, close);
    m.has_matter = true;
    // 行頭コメントだけの block は空 (gray-matter と同じ)。
    bool empty = true;
    for (const auto& l : str::split_lines(block)) {
        const std::string t = str::trim(l);
        if (!t.empty() && t[0] != '#') {
            empty = false;
            break;
        }
    }
    if (!empty) {
        auto data = parse_yaml(block, error, limits);
        if (!data) return std::nullopt;
        if (data->is_object()) m.data = std::move(*data);
        else if (!data->is_null()) {
            // gray-matter は object 以外もそのまま data に入れるが、利用側は object を前提にしている。
            if (error) *error = "front matter is not a mapping";
            return std::nullopt;
        }
    }
    if (!closed) {
        m.content.clear();
    } else {
        std::string content = str_.substr(close + 4);
        if (!content.empty() && content[0] == '\r') content.erase(0, 1);
        if (!content.empty() && content[0] == '\n') content.erase(0, 1);
        m.content = std::move(content);
    }
    return m;
}

} // namespace di::lib::front_matter
