#include "flow/paper_markdown.hpp"

#include <map>

#include "base/strings.hpp"

namespace di::flow {

namespace {

constexpr std::string_view HEAD_THEME = "# 議題";
constexpr std::string_view HEAD_SUPPLEMENT = "# 観点補足";
constexpr std::string_view HEAD_MECHANICS = "# ゲームのメカニクス";
constexpr std::string_view HEAD_GAME_TITLE = "# ゲームタイトル（または主目的）";
constexpr std::string_view HEAD_DISCUSSION_THEME = "# 議論したいテーマ";
constexpr std::string_view HEAD_DISCUSSION_CONTENT = "# 議論内容";
constexpr std::string_view HEAD_MECHANICS_CONTEXT = "# システム/メカニクスの説明";
constexpr std::string_view HEAD_THEME_SUPPLEMENT = "# テーマについての補足情報";
constexpr std::string_view HEAD_EXTRACTED_MECHANICS = "## 抽出されたメカニクス";
constexpr std::string_view NO_MECHANICS = "(メカニクスデータなし)";
constexpr std::string_view HEAD_PROGRESS = "# 議論の経過";
constexpr std::string_view HEAD_CONCLUSION = "# 結論";

/** JS の `.` が一致しない行終端 (\n \r U+2028 U+2029)。 */
bool is_line_terminator(std::uint32_t cp) { return cp == '\n' || cp == '\r' || cp == 0x2028 || cp == 0x2029; }

bool has_line_terminator(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size())
        if (is_line_terminator(str::next_codepoint(s, i))) return true;
    return false;
}

/** s.split(/\r?\n/) */
std::vector<std::string> split_crlf(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const auto nl = s.find('\n', start);
        std::string_view piece = s.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        if (nl != std::string_view::npos && !piece.empty() && piece.back() == '\r') piece.remove_suffix(1);
        out.emplace_back(piece);
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return out;
}

/** /^#\s+(.+?)\s*$/ の捕捉 (一致しなければ nullopt)。 */
std::optional<std::string> match_h1(std::string_view line) {
    if (line.empty() || line[0] != '#') return std::nullopt;
    const std::string_view rest = line.substr(1);
    // 先頭の空白の連続を code point 単位で数える。
    std::vector<std::pair<std::size_t, std::uint32_t>> ws;  // (開始バイト位置, cp)
    std::size_t i = 0;
    while (i < rest.size()) {
        const std::size_t at = i;
        const auto cp = str::next_codepoint(rest, i);
        if (!str::is_js_space(cp)) {
            i = at;
            break;
        }
        ws.emplace_back(at, cp);
    }
    if (ws.empty()) return std::nullopt;
    const std::string_view tail = rest.substr(i);
    if (!tail.empty()) {
        std::string captured = str::trim_end(tail);
        if (has_line_terminator(captured)) return std::nullopt;
        return captured;
    }
    // 全部空白: \s+ を 1 文字以上残して (.+?) が空白 1 文字を取る (行終端は取れない)。
    for (std::size_t j = ws.size(); j-- > 1;) {
        if (is_line_terminator(ws[j].second)) continue;
        std::string out;
        str::append_utf8(out, ws[j].second);
        return out;
    }
    return std::nullopt;
}

using Sections = std::map<std::string, std::vector<std::string>, std::less<>>;

/** md を `# 見出し` ごとのセクションに割る。見出し前の本文は "" キーへ。 */
Sections split_sections(std::string_view md) {
    Sections sections;
    std::string current;
    sections[current];
    for (const auto& raw : split_crlf(md)) {
        if (auto h1 = match_h1(raw)) {
            current = "# " + *h1;
            sections[current];
            continue;
        }
        sections[current].push_back(raw);
    }
    return sections;
}

bool has_section(const Sections& s, std::string_view heading) { return s.find(heading) != s.end(); }

std::string section_text(const Sections& s, std::string_view heading) {
    auto it = s.find(heading);
    if (it == s.end()) return "";
    return str::trim(str::join(it->second, "\n"));
}

std::string mechanic_line(const MechanicSummary& m, bool with_source) {
    const std::string affect =
        (m.intended_affect && !m.intended_affect->empty()) ? " → 期待感情: " + *m.intended_affect : "";
    const std::string source = with_source ? " （出所: " + m.source.value_or("curated") + "）" : "";
    return "- **" + m.name + "**: " + m.description + affect + source;
}

bool truthy(const std::optional<std::string>& s) { return s && !s->empty(); }

bool has_fixed_fields(const PaperContent& c) {
    return truthy(c.gameTitle) || truthy(c.discussionTheme) || truthy(c.discussionContent) ||
           truthy(c.mechanicsContext) || truthy(c.themeSupplement);
}

PaperFixedFields fixed_fields_from_content(const PaperContent& c) {
    PaperFixedFields f;
    f.gameTitle = c.gameTitle ? str::trim(*c.gameTitle) : "";
    const std::string theme = c.discussionTheme ? str::trim(*c.discussionTheme) : "";
    f.discussionTheme = theme.empty() ? c.theme : theme;
    f.discussionContent = c.discussionContent ? str::trim(*c.discussionContent) : "";
    f.mechanicsContext = c.mechanicsContext ? str::trim(*c.mechanicsContext) : "";
    f.themeSupplement = c.themeSupplement ? str::trim(*c.themeSupplement) : c.supplement;
    return f;
}

std::string strip_generated_mechanics(std::string_view text) {
    const auto idx = text.find(HEAD_EXTRACTED_MECHANICS);
    return str::trim(idx == std::string_view::npos ? text : text.substr(0, idx));
}

std::string fixed_mechanics_section(const PaperContent& c) {
    std::vector<std::string> chunks;
    const std::string context = c.mechanicsContext ? str::trim(*c.mechanicsContext) : "";
    if (!context.empty()) chunks.push_back(context);
    if (!grounded_mechanics(c.mechanics).empty())
        chunks.push_back(std::string(HEAD_EXTRACTED_MECHANICS) + "\n" + grounded_mechanics_markdown(c.mechanics));
    if (chunks.empty()) chunks.emplace_back(NO_MECHANICS);
    return str::join(chunks, "\n\n");
}

std::string fixed_paper_to_markdown(const PaperContent& c) {
    const auto fields = fixed_fields_from_content(c);
    const auto issues = issues_section_markdown(c.issues);
    const auto hypothesis = hypothesis_section_markdown(c.mechanics);
    std::vector<std::string> parts{std::string(HEAD_GAME_TITLE) + "\n" + fields.gameTitle,
                                   std::string(HEAD_DISCUSSION_THEME) + "\n" + fields.discussionTheme,
                                   std::string(HEAD_DISCUSSION_CONTENT) + "\n" + fields.discussionContent};
    if (issues) parts.push_back(*issues);
    parts.push_back(std::string(HEAD_MECHANICS_CONTEXT) + "\n" + fixed_mechanics_section(c));
    if (hypothesis) parts.push_back(*hypothesis);
    parts.push_back(std::string(HEAD_THEME_SUPPLEMENT) + "\n" + fields.themeSupplement);
    return str::join(parts, "\n\n");
}

bool is_ascii_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/** 末尾の空白 (code point 単位) を飛ばした位置 (バイト)。 */
std::size_t skip_ws_back(std::string_view s, std::size_t end) {
    while (end > 0) {
        std::size_t start = end - 1;
        while (start > 0 && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) --start;
        std::size_t i = start;
        if (!str::is_js_space(str::next_codepoint(s, i)) || i != end) break;
        end = start;
    }
    return end;
}

/** 末尾が suffix で終わるならその開始位置。 */
std::optional<std::size_t> ends_with_at(std::string_view s, std::size_t end, std::string_view suffix) {
    if (end < suffix.size() || s.substr(end - suffix.size(), suffix.size()) != suffix) return std::nullopt;
    return end - suffix.size();
}

std::optional<std::size_t> ends_with_any(std::string_view s, std::size_t end, std::string_view a, std::string_view b) {
    if (auto p = ends_with_at(s, end, a)) return p;
    return ends_with_at(s, end, b);
}

struct SourceSuffix {
    std::string rest;
    std::optional<MechanicSource> source;
};

/** 行末の出所併記 `（出所: curated）` (半角括弧も許容) を取り出して除去する。 */
SourceSuffix extract_source_suffix(const std::string& text) {
    // /[（(]\s*出所\s*[:：]\s*([a-z]+)\s*[）)]\s*$/i を末尾から逆に辿る (構造が一意なので逆走査で同じ一致になる)。
    std::size_t p = skip_ws_back(text, text.size());
    auto close = ends_with_any(text, p, "）", ")");
    if (!close) return {text, std::nullopt};
    p = skip_ws_back(text, *close);
    const std::size_t letters_end = p;
    while (p > 0 && is_ascii_alpha(text[p - 1])) --p;
    if (p == letters_end) return {text, std::nullopt};
    const std::string letters = text.substr(p, letters_end - p);
    p = skip_ws_back(text, p);
    auto colon = ends_with_any(text, p, ":", "：");
    if (!colon) return {text, std::nullopt};
    p = skip_ws_back(text, *colon);
    auto label = ends_with_at(text, p, "出所");
    if (!label) return {text, std::nullopt};
    p = skip_ws_back(text, *label);
    auto open = ends_with_any(text, p, "（", "(");
    if (!open) return {text, std::nullopt};
    auto source = as_mechanic_source(Json(str::to_lower_ascii(letters)));
    if (!source) return {text, std::nullopt};
    return {str::trim(std::string_view(text).substr(0, *open)), source};
}

/** `- **名前**: 説明 → 期待感情: x （出所: y）` を構造化する。 */
std::optional<MechanicSummary> parse_mechanic_line(std::string_view raw_line, const std::optional<MechanicSource>& forced) {
    // /^-\s+\*\*(.+?)\*\*\s*:\s*(.*)$/
    const std::string line = str::trim(raw_line);
    if (line.empty() || line[0] != '-') return std::nullopt;
    std::size_t i = 1;
    std::size_t ws_count = 0;
    while (i < line.size()) {
        std::size_t next = i;
        if (!str::is_js_space(str::next_codepoint(line, next))) break;
        i = next;
        ++ws_count;
    }
    if (ws_count == 0 || line.compare(i, 2, "**") != 0) return std::nullopt;
    const std::size_t name_start = i + 2;
    // (.+?) は 1 文字以上・行終端なし。最短の名前から順に `**\s*:` を探す。
    std::size_t search = name_start;
    while (true) {
        if (search >= line.size()) return std::nullopt;
        // 名前は 1 code point 以上: 最初の候補は name_start の次の code point から。
        std::size_t first_end = name_start;
        str::next_codepoint(line, first_end);
        const auto star = line.find("**", std::max(search, first_end));
        if (star == std::string::npos) return std::nullopt;
        const std::string_view name_view = std::string_view(line).substr(name_start, star - name_start);
        if (has_line_terminator(name_view)) return std::nullopt;
        std::size_t j = star + 2;
        while (j < line.size()) {
            std::size_t next = j;
            if (!str::is_js_space(str::next_codepoint(line, next))) break;
            j = next;
        }
        if (j < line.size() && line[j] == ':') {
            std::size_t k = j + 1;
            while (k < line.size()) {
                std::size_t next = k;
                if (!str::is_js_space(str::next_codepoint(line, next))) break;
                k = next;
            }
            const std::string_view rest_view = std::string_view(line).substr(k);
            if (has_line_terminator(rest_view)) return std::nullopt;
            const std::string name = str::trim(name_view);
            if (name.empty()) return std::nullopt;
            // 出所併記 (08) を先に剥がす → 期待感情 → 説明 の順で分解する。
            auto [no_source, parsed_source] = extract_source_suffix(str::trim(rest_view));
            std::string rest = no_source;
            std::optional<std::string> intended_affect;
            const std::string marker = "→ 期待感情:";
            const auto affect_idx = rest.find(marker);
            if (affect_idx != std::string::npos) {
                const std::string affect = str::trim(std::string_view(rest).substr(affect_idx + marker.size()));
                if (!affect.empty()) intended_affect = affect;
                rest = str::trim(std::string_view(rest).substr(0, affect_idx));
            }
            MechanicSummary m;
            m.name = name;
            m.description = rest;
            m.intended_affect = intended_affect;
            m.source = forced ? forced : parsed_source;
            return m;
        }
        search = star + 1;
    }
}

std::vector<MechanicSummary> parse_mechanics(const std::string& text, const std::optional<MechanicSource>& forced = std::nullopt) {
    if (text.empty() || text == NO_MECHANICS) return {};
    std::vector<MechanicSummary> out;
    for (const auto& line : split_crlf(text))
        if (auto m = parse_mechanic_line(line, forced)) out.push_back(std::move(*m));
    return out;
}

bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

/** 末尾の `(両論: ○)` 注記 (/\s*[（(]両論\s*[:：][^)）]*[)）]\s*$/) を剥がす。 */
std::string strip_armable_note(const std::string& s) {
    const std::size_t end = skip_ws_back(s, s.size());
    auto close = ends_with_any(s, end, "）", ")");
    if (!close) return s;
    // [^)）]* は閉じ括弧を含められない → 開き括弧は直前の閉じ括弧より後ろ。
    std::size_t boundary = 0;
    {
        const std::string_view head = std::string_view(s).substr(0, *close);
        const auto a = head.rfind(')');
        const auto b = head.rfind("）");
        std::size_t last = std::string::npos;
        if (a != std::string_view::npos) last = a;
        if (b != std::string_view::npos && (last == std::string::npos || b > last)) last = b;
        if (last != std::string::npos) boundary = last + (last == a ? 1 : std::string_view("）").size());
    }
    for (std::size_t p = boundary; p < *close;) {
        std::size_t open_len = 0;
        if (s.compare(p, 1, "(") == 0) open_len = 1;
        else if (s.compare(p, 3, "（") == 0) open_len = 3;
        if (open_len > 0) {
            std::size_t q = p + open_len;
            if (s.compare(q, 6, "両論") == 0) {
                q += 6;
                while (q < *close) {
                    std::size_t next = q;
                    if (!str::is_js_space(str::next_codepoint(s, next))) break;
                    q = next;
                }
                if (s.compare(q, 1, ":") == 0 || s.compare(q, 3, "：") == 0) {
                    // 一致の開始は直前の空白の連続の先頭。
                    const std::size_t start = skip_ws_back(s, p);
                    return s.substr(0, start);
                }
            }
        }
        str::next_codepoint(s, p);
    }
    return s;
}

/** 論点セクション本文 → issues[]。`1. 論点` / `- 論点` の両形式を受ける。 */
std::vector<std::string> parse_issues(const std::string& text) {
    if (text.empty()) return {};
    std::vector<std::string> out;
    for (const auto& raw : split_crlf(text)) {
        const std::string line = str::trim(raw);
        // /^(?:\d+[.)]\s+|[-*]\s+)(.+)$/
        std::size_t i = 0;
        if (i < line.size() && is_ascii_digit(line[i])) {
            while (i < line.size() && is_ascii_digit(line[i])) ++i;
            if (i >= line.size() || (line[i] != '.' && line[i] != ')')) continue;
            ++i;
        } else if (i < line.size() && (line[i] == '-' || line[i] == '*')) {
            ++i;
        } else {
            continue;
        }
        std::size_t ws = 0;
        while (i < line.size()) {
            std::size_t next = i;
            if (!str::is_js_space(str::next_codepoint(line, next))) break;
            i = next;
            ++ws;
        }
        if (ws == 0 || i >= line.size()) continue;
        const std::string captured = line.substr(i);
        if (has_line_terminator(captured)) continue;
        const std::string issue = str::trim(strip_armable_note(captured));
        if (!issue.empty()) out.push_back(issue);
    }
    return out;
}

} // namespace

Json to_json(const RoundSummary& r) {
    Json o = Json::object();
    o["round"] = r.round;
    o["summary"] = r.summary;
    o["aufhebung"] = r.aufhebung;
    return o;
}

Json to_json(const PaperContent& c) {
    Json o = Json::object();
    o["theme"] = c.theme;
    o["tags"] = c.tags;
    o["supplement"] = c.supplement;
    Json mech = Json::array();
    for (const auto& m : c.mechanics) mech.push_back(to_json(m));
    o["mechanics"] = std::move(mech);
    if (c.issues) o["issues"] = *c.issues;
    if (c.gameTitle) o["gameTitle"] = *c.gameTitle;
    if (c.discussionTheme) o["discussionTheme"] = *c.discussionTheme;
    if (c.discussionContent) o["discussionContent"] = *c.discussionContent;
    if (c.mechanicsContext) o["mechanicsContext"] = *c.mechanicsContext;
    if (c.themeSupplement) o["themeSupplement"] = *c.themeSupplement;
    return o;
}

PaperContent paper_content_from_json(const Json& j) {
    PaperContent c;
    c.theme = json_string(j, "theme");
    for (const auto& t : json_array(j, "tags"))
        if (t.is_string()) c.tags.push_back(t.get<std::string>());
    c.supplement = json_string(j, "supplement");
    for (const auto& m : json_array(j, "mechanics")) c.mechanics.push_back(mechanic_summary_from_json(m));
    if (j.is_object() && j.contains("issues") && j["issues"].is_array()) {
        std::vector<std::string> issues;
        for (const auto& s : j["issues"])
            if (s.is_string()) issues.push_back(s.get<std::string>());
        c.issues = std::move(issues);
    }
    c.gameTitle = json_opt_string(j, "gameTitle");
    c.discussionTheme = json_opt_string(j, "discussionTheme");
    c.discussionContent = json_opt_string(j, "discussionContent");
    c.mechanicsContext = json_opt_string(j, "mechanicsContext");
    c.themeSupplement = json_opt_string(j, "themeSupplement");
    return c;
}

Json to_json(const PaperFixedFields& f) {
    Json o = Json::object();
    o["gameTitle"] = f.gameTitle;
    o["discussionTheme"] = f.discussionTheme;
    o["discussionContent"] = f.discussionContent;
    o["mechanicsContext"] = f.mechanicsContext;
    o["themeSupplement"] = f.themeSupplement;
    return o;
}

std::vector<MechanicSummary> grounded_mechanics(const std::vector<MechanicSummary>& mechanics) {
    std::vector<MechanicSummary> out;
    for (const auto& m : mechanics)
        if (m.source != std::optional<MechanicSource>("llm")) out.push_back(m);
    return out;
}

std::vector<MechanicSummary> hypothetical_mechanics(const std::vector<MechanicSummary>& mechanics) {
    std::vector<MechanicSummary> out;
    for (const auto& m : mechanics)
        if (m.source == std::optional<MechanicSource>("llm")) out.push_back(m);
    return out;
}

std::string grounded_mechanics_markdown(const std::vector<MechanicSummary>& mechanics) {
    const auto grounded = grounded_mechanics(mechanics);
    if (grounded.empty()) return std::string(NO_MECHANICS);
    std::vector<std::string> lines;
    for (const auto& m : grounded) lines.push_back(mechanic_line(m, true));
    return str::join(lines, "\n");
}

std::optional<std::string> hypothesis_section_markdown(const std::vector<MechanicSummary>& mechanics) {
    const auto hyp = hypothetical_mechanics(mechanics);
    if (hyp.empty()) return std::nullopt;
    std::vector<std::string> lines;
    for (const auto& m : hyp) lines.push_back(mechanic_line(m, false));
    return std::string(HEAD_HYPOTHESIS_MECHANICS) + "\n" + str::join(lines, "\n");
}

std::string mechanics_body_markdown(const std::vector<MechanicSummary>& mechanics) {
    const auto hypothesis = hypothesis_section_markdown(mechanics);
    const auto grounded = grounded_mechanics_markdown(mechanics);
    return hypothesis ? grounded + "\n\n" + *hypothesis : grounded;
}

std::optional<std::string> issues_section_markdown(const std::optional<std::vector<std::string>>& issues) {
    std::vector<std::string> list;
    if (issues)
        for (const auto& s : *issues) {
            auto t = str::trim(s);
            if (!t.empty()) list.push_back(std::move(t));
        }
    if (list.empty()) return std::nullopt;
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < list.size(); ++i) lines.push_back(std::to_string(i + 1) + ". " + list[i]);
    return std::string(HEAD_ISSUES) + "\n" + str::join(lines, "\n");
}

std::string paper_draft_to_markdown(const PaperContent& content) {
    if (has_fixed_fields(content)) return fixed_paper_to_markdown(content);
    std::vector<std::string> sections{std::string(HEAD_THEME) + "\n" + content.theme};
    if (!content.supplement.empty()) sections.push_back(std::string(HEAD_SUPPLEMENT) + "\n" + content.supplement);
    if (auto issues = issues_section_markdown(content.issues)) sections.push_back(*issues);
    // メカニクス本文 (grounded + 仮説節) — 仮説は source=llm のみ、0 件なら節を出さない。
    sections.push_back(std::string(HEAD_MECHANICS) + "\n" + mechanics_body_markdown(content.mechanics));
    return str::join(sections, "\n\n");
}

PaperFixedFields paper_fixed_fields_from_markdown(std::string_view md, const PaperContent& fallback) {
    const auto sections = split_sections(md);
    auto pick = [](std::initializer_list<std::string> candidates) {
        for (const auto& c : candidates)
            if (!c.empty()) return c;
        return std::string();
    };
    const std::string mechanics_text = section_text(sections, HEAD_MECHANICS_CONTEXT);
    PaperFixedFields f;
    f.gameTitle = pick({section_text(sections, HEAD_GAME_TITLE), fallback.gameTitle.value_or("")});
    f.discussionTheme = pick({section_text(sections, HEAD_DISCUSSION_THEME), fallback.discussionTheme.value_or(""),
                              fallback.theme});
    f.discussionContent =
        pick({section_text(sections, HEAD_DISCUSSION_CONTENT), fallback.discussionContent.value_or("")});
    f.mechanicsContext = pick({strip_generated_mechanics(mechanics_text), fallback.mechanicsContext.value_or("")});
    f.themeSupplement = pick({section_text(sections, HEAD_THEME_SUPPLEMENT), fallback.themeSupplement.value_or(""),
                              fallback.supplement});
    return f;
}

PaperContent markdown_to_paper_draft(std::string_view md, const PaperContent& fallback) {
    const auto sections = split_sections(md);
    const bool is_fixed = has_section(sections, HEAD_GAME_TITLE) || has_section(sections, HEAD_DISCUSSION_THEME);

    // 仮説メカニクス節 (source=llm)。節が在ればその内容が正、無ければ「仮説なし」。
    const bool hyp_has = has_section(sections, HEAD_HYPOTHESIS_MECHANICS);
    const auto hyp_mechanics = hyp_has ? parse_mechanics(section_text(sections, HEAD_HYPOTHESIS_MECHANICS), "llm")
                                       : std::vector<MechanicSummary>{};
    // 論点節: 見出しが在れば (空にされた場合も含め) その値、無ければ fallback。
    const bool issues_has = has_section(sections, HEAD_ISSUES);
    const std::optional<std::vector<std::string>> issues =
        issues_has ? std::optional<std::vector<std::string>>(parse_issues(section_text(sections, HEAD_ISSUES)))
                   : fallback.issues;

    /** grounded 節の有無/内容と仮説節から mechanics を合成する共通則。 */
    auto merge = [&](bool grounded_has, const std::string& grounded_text) {
        // どちらの見出しも消された自由編集では fallback を保つ (付随機能の取りこぼし防止)。
        if (!grounded_has && !hyp_has) return fallback.mechanics;
        auto grounded = grounded_has ? parse_mechanics(grounded_text) : grounded_mechanics(fallback.mechanics);
        grounded.insert(grounded.end(), hyp_mechanics.begin(), hyp_mechanics.end());
        return grounded;
    };

    if (is_fixed) {
        const auto fields = paper_fixed_fields_from_markdown(md, fallback);
        PaperContent out = fallback;
        out.gameTitle = fields.gameTitle;
        out.discussionTheme = fields.discussionTheme;
        out.discussionContent = fields.discussionContent;
        out.mechanicsContext = fields.mechanicsContext;
        out.themeSupplement = fields.themeSupplement;
        out.theme = fields.discussionTheme.empty() ? fallback.theme : fields.discussionTheme;
        out.tags = fallback.tags;
        out.supplement = has_section(sections, HEAD_THEME_SUPPLEMENT) ? fields.themeSupplement : fallback.supplement;
        out.mechanics = merge(has_section(sections, HEAD_MECHANICS_CONTEXT), section_text(sections, HEAD_MECHANICS_CONTEXT));
        out.issues = issues;
        return out;
    }
    PaperContent out;
    const std::string theme_text = section_text(sections, HEAD_THEME);
    out.theme = theme_text.empty() ? fallback.theme : theme_text;
    out.tags = fallback.tags;
    // 観点補足の見出しが在れば (空にされた場合も含め) その値、無ければ fallback。
    out.supplement = has_section(sections, HEAD_SUPPLEMENT) ? section_text(sections, HEAD_SUPPLEMENT) : fallback.supplement;
    out.mechanics = merge(has_section(sections, HEAD_MECHANICS), section_text(sections, HEAD_MECHANICS));
    out.issues = issues;
    return out;
}

std::string strip_progress(std::string_view body_md) {
    const std::string marker = "\n" + std::string(HEAD_PROGRESS);
    const auto idx = body_md.find(marker);
    return str::trim_end(idx == std::string_view::npos ? body_md : body_md.substr(0, idx));
}

std::string render_progress_markdown(std::string_view base_body_md, const std::vector<RoundSummary>& rounds,
                                     const std::optional<std::string>& conclusion) {
    std::vector<std::string> parts{strip_progress(base_body_md)};
    if (!rounds.empty()) {
        std::vector<std::string> blocks;
        for (const auto& r : rounds) {
            const std::string summary = str::trim(r.summary);
            std::vector<std::string> lines{"## ラウンド " + std::to_string(r.round), summary.empty() ? "(まとめなし)" : summary};
            if (!r.aufhebung.empty()) {
                lines.emplace_back("");
                lines.emplace_back("**止揚 (アウフヘーベン)**:");
                for (const auto& a : r.aufhebung) lines.push_back("- " + a);
            }
            blocks.push_back(str::join(lines, "\n"));
        }
        parts.push_back(std::string(HEAD_PROGRESS) + "\n\n" + str::join(blocks, "\n\n"));
    }
    if (conclusion && !str::trim(*conclusion).empty())
        parts.push_back(std::string(HEAD_CONCLUSION) + "\n" + str::trim(*conclusion));
    return str::join(parts, "\n\n");
}

} // namespace di::flow
