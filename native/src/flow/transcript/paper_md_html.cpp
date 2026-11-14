#include "flow/transcript/paper_md_html.hpp"

#include <optional>
#include <vector>

#include "base/strings.hpp"

namespace di::flow::transcript {

namespace {

/** s[i..] の先頭の JS \s をいくつ読み飛ばしたか (バイト数)。 */
std::size_t skip_spaces(std::string_view s, std::size_t i) {
    std::size_t j = i;
    while (j < s.size()) {
        std::size_t k = j;
        if (!str::is_js_space(str::next_codepoint(s, k))) break;
        j = k;
    }
    return j - i;
}

/** line が marker + 1 文字以上の空白 + 本文 なら本文を返す (JS の /^marker\s+(.+)$/)。 */
std::optional<std::string_view> after_marker(std::string_view line, std::string_view marker) {
    if (!str::starts_with(line, marker)) return std::nullopt;
    const std::size_t sp = skip_spaces(line, marker.size());
    if (sp == 0) return std::nullopt;
    const auto rest = line.substr(marker.size() + sp);
    if (rest.empty()) return std::nullopt;
    return rest;
}

/** 「- 」「* 」の箇条書きなら本文を返す (JS の /^[-*]\s+(.+)$/)。 */
std::optional<std::string_view> list_item(std::string_view line) {
    if (auto li = after_marker(line, "-")) return li;
    return after_marker(line, "*");
}

/** エスケープしてから **太字** を <strong> にする (JS の /\*\*(.+?)\*\*\/g)。 */
std::string inline_html(std::string_view raw) {
    const std::string s = str::escape_html(raw);
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto open = s.find("**", i);
        if (open == std::string::npos) break;
        const auto close = s.find("**", open + 3);
        if (close == std::string::npos) break;
        out.append(s, i, open - i);
        out += "<strong>";
        out.append(s, open + 2, close - open - 2);
        out += "</strong>";
        i = close + 2;
    }
    out.append(s, i, std::string::npos);
    return out;
}

} // namespace

std::string render_paper_md_html(std::string_view md) {
    std::string html;
    bool in_list = false;
    const auto close_list = [&] {
        if (in_list) {
            html += "</ul>";
            in_list = false;
        }
    };
    for (const auto& raw : str::split(md, "\n")) {
        const std::string line = str::trim_end(raw);
        if (line.empty()) {
            close_list();
            continue;
        }
        if (const auto h3 = after_marker(line, "#")) {
            close_list();
            html += "<h3>" + inline_html(*h3) + "</h3>";
        } else if (const auto h4 = after_marker(line, "##")) {
            close_list();
            html += "<h4>" + inline_html(*h4) + "</h4>";
        } else if (const auto li = list_item(line)) {
            if (!in_list) {
                html += "<ul>";
                in_list = true;
            }
            html += "<li>" + inline_html(*li) + "</li>";
        } else {
            close_list();
            html += "<p>" + inline_html(line) + "</p>";
        }
    }
    close_list();
    return html;
}

} // namespace di::flow::transcript
