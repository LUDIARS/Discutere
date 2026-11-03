// src/flow/entry-discord.ts の移植。正規表現は線形の走査で書く (std::regex は使わない)。
#include "flow/entry_discord.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "base/strings.hpp"

namespace di::flow {

namespace {

const std::vector<std::string>& default_flow_tag_names() {
    static const std::vector<std::string> v = {"機密", "内部", "運用", "開発", "未来予測", "代理"};
    return v;
}

bool name_hits(const std::string& trimmed, const std::string& t) { return trimmed == t || str::contains(trimmed, t); }

bool is_ascii_word(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/** text[pos..] が ASCII の kw (小文字) と大文字小文字を無視して一致するか。 */
bool ascii_ci_at(std::string_view text, std::size_t pos, std::string_view kw) {
    if (pos + kw.size() > text.size()) return false;
    for (std::size_t i = 0; i < kw.size(); ++i)
        if (ascii_lower(text[pos + i]) != kw[i]) return false;
    return true;
}

/** pos から JS の \s を読み飛ばした位置。 */
std::size_t skip_spaces(std::string_view text, std::size_t pos) {
    while (pos < text.size()) {
        std::size_t next = pos;
        const auto cp = str::next_codepoint(text, next);
        if (!str::is_js_space(cp)) break;
        pos = next;
    }
    return pos;
}

/** [:：=] を読んだ後の位置。一致しなければ npos。 */
std::size_t match_sep(std::string_view text, std::size_t pos) {
    if (pos < text.size() && (text[pos] == ':' || text[pos] == '=')) return pos + 1;
    static constexpr std::string_view kFullColon = "：";
    if (text.substr(pos, kFullColon.size()) == kFullColon) return pos + kFullColon.size();
    return std::string_view::npos;
}

/** keyword の直後 q からの「\s*[:：=]\s*(\d+)」。一致すれば数字列。 */
std::optional<std::string> match_sep_digits(std::string_view text, std::size_t q) {
    q = skip_spaces(text, q);
    q = match_sep(text, q);
    if (q == std::string_view::npos) return std::nullopt;
    q = skip_spaces(text, q);
    std::size_t end = q;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
    if (end == q) return std::nullopt;
    return std::string(text.substr(q, end - q));
}

/** /<kana>\s*[:：=]\s*(\d+)/i の最初の一致の m[1]。 */
std::optional<std::string> first_kana_match(std::string_view text, std::string_view kana) {
    for (std::size_t pos = text.find(kana); pos != std::string_view::npos; pos = text.find(kana, pos + 1)) {
        if (auto d = match_sep_digits(text, pos + kana.size())) return d;
    }
    return std::nullopt;
}

/** /\b<word>s?\s*[:：=]\s*(\d+)/i の最初の一致の m[1] (word は小文字の ASCII)。 */
std::optional<std::string> first_word_match(std::string_view text, std::string_view word) {
    for (std::size_t pos = 0; pos + word.size() <= text.size(); ++pos) {
        if (!ascii_ci_at(text, pos, word)) continue;
        // \b: 直前が単語文字なら境界ではない (非 ASCII は単語文字ではない)
        if (pos > 0 && is_ascii_word(static_cast<unsigned char>(text[pos - 1]))) continue;
        const std::size_t q = pos + word.size();
        // s? は貪欲 (付けて試し、駄目なら付けずに試す)
        if (q < text.size() && ascii_lower(text[q]) == 's') {
            if (auto d = match_sep_digits(text, q + 1)) return d;
        }
        if (auto d = match_sep_digits(text, q)) return d;
    }
    return std::nullopt;
}

/** Number(m[1]) が有限なら値 (TS の pick)。 */
std::optional<double> finite_number(const std::optional<std::string>& digits) {
    if (!digits) return std::nullopt;
    const double n = std::strtod(digits->c_str(), nullptr);
    if (!std::isfinite(n)) return std::nullopt;
    return n;
}

std::optional<double> pick(std::string_view text, std::string_view kana, std::string_view word) {
    // 各パターンの最初の一致だけを見る。数値が有限でなければ次のパターンへ (TS と同じ)。
    if (auto n = finite_number(first_kana_match(text, kana))) return n;
    if (auto n = finite_number(first_word_match(text, word))) return n;
    return std::nullopt;
}

bool is_line_terminator(std::uint32_t cp) { return cp == '\n' || cp == '\r' || cp == 0x2028 || cp == 0x2029; }

/** keyword 直後 q からの「\s*[:：=]\s*(.+)」の m[1]。 */
std::optional<std::string> match_opponent_rest(std::string_view text, std::size_t q) {
    q = skip_spaces(text, q);
    q = match_sep(text, q);
    if (q == std::string_view::npos) return std::nullopt;
    // \s* は貪欲。最長から縮めて、直後に行末記号以外の 1 文字がある位置を探す。
    std::vector<std::size_t> starts{q};
    std::size_t p = q;
    while (p < text.size()) {
        std::size_t next = p;
        const auto cp = str::next_codepoint(text, next);
        if (!str::is_js_space(cp)) break;
        p = next;
        starts.push_back(p);
    }
    for (auto it = starts.rbegin(); it != starts.rend(); ++it) {
        const std::size_t s = *it;
        std::size_t e = s;
        while (e < text.size()) {
            std::size_t next = e;
            const auto cp = str::next_codepoint(text, next);
            if (is_line_terminator(cp)) break;
            e = next;
        }
        if (e > s) return std::string(text.substr(s, e - s));
    }
    return std::nullopt;
}

} // namespace

Json to_json(const ParsedForumEntry& p) {
    Json j = Json::object();
    j["flow"] = p.flow ? Json(*p.flow) : Json(nullptr);
    j["tags"] = p.tags;
    return j;
}

Json to_json(const RoundsTurns& r) {
    Json j = Json::object();
    if (r.rounds) j["rounds"] = *r.rounds;
    if (r.turnsPerRound) j["turnsPerRound"] = *r.turnsPerRound;
    return j;
}

ParsedForumEntry parse_forum_entry(const std::vector<std::string>& applied_tag_names, const std::optional<ForumTagMapping>& mapping) {
    const std::vector<std::string>& flow_tag_names =
        mapping && mapping->flowTagNames ? *mapping->flowTagNames : default_flow_tag_names();

    ParsedForumEntry out;
    // フロータグ抽出 (タグ名が FlowTag と一致、または含む)
    for (const auto& name : applied_tag_names) {
        const std::string trimmed = str::trim(name);
        for (const auto& t : flow_tag_names) {
            if (!name_hits(trimmed, t)) continue;
            // TS の `if (hit && ...)`: 空文字のタグ名に当たったら何も足さない
            if (t.empty()) break;
            if (std::find(out.tags.begin(), out.tags.end(), t) == out.tags.end()) out.tags.push_back(t);
            break;
        }
    }

    // 議論タイプ判定: まず override の別名、次に標準 parse_flow_kind
    for (const auto& name : applied_tag_names) {
        if (mapping && mapping->flowKindTagNames) {
            const std::string trimmed = str::trim(name);
            const std::pair<FlowKind, std::vector<std::string>>* matched = nullptr;
            for (const auto& entry : *mapping->flowKindTagNames) {
                bool any = false;
                for (const auto& n : entry.second)
                    if (name_hits(trimmed, n)) {
                        any = true;
                        break;
                    }
                if (any) {
                    matched = &entry;
                    break;
                }
            }
            if (matched && (matched->first == "discussion" || matched->first == "improvement")) {
                out.flow = matched->first;
                break;
            }
        }
        if (auto k = parse_flow_kind(std::string_view(name))) {
            out.flow = *k;
            break;
        }
    }
    return out;
}

RoundsTurns parse_rounds_turns(std::string_view text) {
    RoundsTurns r;
    r.rounds = pick(text, "ラウンド", "round");
    r.turnsPerRound = pick(text, "ターン", "turn");
    return r;
}

std::optional<std::vector<std::string>> parse_opponents(std::string_view text) {
    static constexpr std::string_view kAite = "相手";
    std::optional<std::string> rest;
    for (std::size_t pos = 0; pos < text.size() && !rest; ++pos) {
        if (text.substr(pos, kAite.size()) == kAite) {
            rest = match_opponent_rest(text, pos + kAite.size());
            if (rest) break;
        }
        if (ascii_ci_at(text, pos, "opponent")) rest = match_opponent_rest(text, pos + 8);
    }
    if (!rest) return std::nullopt;
    // 行末まで取り (. は行末記号を含まない)、カンマ/読点で分解する。
    std::vector<std::string> ids;
    std::string cur;
    auto flush = [&] {
        auto t = str::trim(cur);
        if (!t.empty()) ids.push_back(std::move(t));
        cur.clear();
    };
    static constexpr std::string_view kTouten = "、";
    for (std::size_t i = 0; i < rest->size();) {
        if ((*rest)[i] == ',') {
            flush();
            ++i;
        } else if (std::string_view(*rest).substr(i, kTouten.size()) == kTouten) {
            flush();
            i += kTouten.size();
        } else {
            cur.push_back((*rest)[i++]);
        }
    }
    flush();
    if (ids.empty()) return std::nullopt;
    return ids;
}

ForumEntryResult handle_forum_flow_post(const ForumPostInput& input, const DispatchDeps& deps, const std::optional<ForumTagMapping>& mapping) {
    const auto parsed = parse_forum_entry(input.appliedTagNames, mapping);
    ForumEntryResult r;
    if (!parsed.flow) {
        r.ok = false;
        r.reason = "flow-required";
        r.message = std::string(FLOW_REQUIRED_MESSAGE);
        return r;
    }
    DispatchInput in;
    in.theme = input.theme;
    in.tags = parsed.tags;
    in.flow = *parsed.flow;
    in.scene = input.scene;
    r.ok = true;
    r.dispatch = dispatch_flow(in, deps);
    return r;
}

} // namespace di::flow
