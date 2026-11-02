#include "visualize/conclusion_markdown.hpp"

#include <stdexcept>
#include <vector>

#include "base/strings.hpp"
#include "base/time.hpp"
#include "lib/front_matter/front_matter.hpp"

namespace di::visualize {

namespace {

/** /[\\/:*?"<>|#%{}\s]/ の 1 文字。 */
bool is_slug_break(std::uint32_t cp) {
    switch (cp) {
        case '\\': case '/': case ':': case '*': case '?': case '"': case '<': case '>': case '|': case '#': case '%':
        case '{': case '}':
            return true;
        default:
            return str::is_js_space(cp);
    }
}

/** 出所サフィックス (§6: source/sourceUrl は開示)。無ければ空。 */
std::string source_suffix(const std::optional<std::string>& source, const std::optional<std::string>& source_url) {
    if (!source || source->empty()) return "";
    if (source_url && !source_url->empty()) return " ([" + *source + "](" + *source_url + "))";
    return " (" + *source + ")";
}

/** Date の範囲 (±8.64e15 ms)。 */
constexpr std::int64_t kMaxDateMs = 8'640'000'000'000'000LL;

} // namespace

std::string safe_slug(const std::string& title, const std::string& fallback) {
    std::string replaced;
    bool in_break = false;
    std::size_t i = 0;
    while (i < title.size()) {
        const std::size_t start = i;
        const std::uint32_t cp = str::next_codepoint(title, i);
        if (is_slug_break(cp)) {
            if (!in_break) replaced += '-';
            in_break = true;
        } else {
            replaced.append(title, start, i - start);
            in_break = false;
        }
    }
    std::size_t b = 0, e = replaced.size();
    while (b < e && replaced[b] == '-') ++b;
    while (e > b && replaced[e - 1] == '-') --e;
    const std::string slug = str::js_slice(std::string_view(replaced).substr(b, e - b), 0, 60);
    return slug.empty() ? fallback : slug;
}

std::string render_conclusion_markdown(const ConclusionDetail& detail) {
    if (detail.updatedAt > kMaxDateMs || detail.updatedAt < -kMaxDateMs) throw std::range_error("Invalid time value");
    Json fm = Json::object();
    fm["type"] = "discussion-conclusion";
    fm["kind"] = detail.kind;
    fm["id"] = detail.gapId;
    fm["session"] = detail.sessionId;
    fm["title"] = detail.title;
    fm["concluded"] = detail.conclusion.has_value();
    fm["utterances"] = detail.utteranceCount;
    fm["aufhebungen"] = detail.aufhebungCount;
    fm["updated_at"] = time::iso_utc(detail.updatedAt);

    std::vector<std::string> lines;
    lines.push_back("# " + detail.title);
    lines.push_back("");
    lines.push_back("## 結論");
    lines.push_back("");
    lines.push_back(detail.conclusion ? *detail.conclusion : "(まとめ未生成)");

    // ディスカッションペーパー (議題ブリーフ)。新フロー結論のみ持つ。
    if (detail.paper) {
        const auto& p = *detail.paper;
        lines.push_back("");
        lines.push_back("## ディスカッションペーパー");
        if (!p.tags.empty()) {
            lines.push_back("");
            lines.push_back("観点タグ: " + str::join(p.tags, " / "));
        }
        if (!p.supplement.empty()) {
            lines.push_back("");
            lines.push_back("### 観点補足");
            lines.push_back("");
            lines.push_back(p.supplement);
        }
        if (!p.mechanics.empty()) {
            lines.push_back("");
            lines.push_back("### ゲームのメカニクス");
            lines.push_back("");
            for (const auto& m : p.mechanics) {
                const std::string affect = m.intendedAffect && !m.intendedAffect->empty() ? " → 期待感情: " + *m.intendedAffect : "";
                lines.push_back("- **" + m.name + "**: " + m.description + affect);
            }
        }
    }

    if (!detail.aufhebungen.empty()) {
        lines.push_back("");
        lines.push_back("## 止揚ストック");
        lines.push_back("");
        for (const auto& a : detail.aufhebungen) lines.push_back("- " + a);
    }

    if (!detail.topOpinions.empty()) {
        lines.push_back("");
        lines.push_back("## 高評価意見");
        lines.push_back("");
        for (const auto& o : detail.topOpinions)
            lines.push_back("- **+" + std::to_string(o.score) + " " + o.speaker + "**: " + o.content + source_suffix(o.source, o.sourceUrl));
    }

    lines.push_back("");
    lines.push_back("## 議論ログ (" + std::to_string(detail.transcript.size()) + " 発話)");
    lines.push_back("");
    for (const auto& u : detail.transcript)
        lines.push_back("- **" + u.speaker + "**: " + u.content + source_suffix(u.source, u.sourceUrl));

    return lib::front_matter::stringify("\n" + str::join(lines, "\n") + "\n", fm);
}

} // namespace di::visualize
