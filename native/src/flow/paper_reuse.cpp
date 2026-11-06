#include "flow/paper_reuse.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "flow/db/connection.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/paper_voices.hpp"

namespace di::flow {

namespace {

bool iequals_at(std::string_view s, std::size_t pos, std::string_view word) {
    if (pos + word.size() > s.size()) return false;
    return str::iequals_ascii(s.substr(pos, word.size()), word);
}

/** ASCII / 全角の数字なら 0..9、それ以外は -1。読んだバイト数を len に。 */
int digit_at(std::string_view s, std::size_t pos, std::size_t& len) {
    if (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        len = 1;
        return s[pos] - '0';
    }
    std::size_t i = pos;
    if (pos < s.size()) {
        const auto cp = str::next_codepoint(s, i);
        if (cp >= 0xFF10 && cp <= 0xFF19) {
            len = i - pos;
            return static_cast<int>(cp - 0xFF10);
        }
    }
    return -1;
}

std::size_t skip_ws(std::string_view s, std::size_t p) {
    while (p < s.size()) {
        std::size_t next = p;
        if (!str::is_js_space(str::next_codepoint(s, next))) break;
        p = next;
    }
    return p;
}

} // namespace

std::optional<std::int64_t> parse_reuse_paper_no(const std::optional<std::string>& text) {
    if (!text || text->empty()) return std::nullopt;
    const std::string_view s = *text;
    const std::string_view kana = "ペーパー";
    // /(?:ペーパー|paper)\s*[#＃]?\s*([0-9０-９]+)/i の最初の一致。
    for (std::size_t pos = 0; pos < s.size(); ++pos) {
        std::size_t after = 0;
        if (s.compare(pos, kana.size(), kana) == 0) after = pos + kana.size();
        else if (iequals_at(s, pos, "paper")) after = pos + 5;
        else continue;
        std::size_t p = skip_ws(s, after);
        if (s.compare(p, 1, "#") == 0) p = skip_ws(s, p + 1);
        else if (s.compare(p, 3, "＃") == 0) p = skip_ws(s, p + 3);
        std::string digits;
        std::size_t len = 0;
        int d;
        while ((d = digit_at(s, p, len)) >= 0) {
            digits.push_back(static_cast<char>('0' + d));
            p += len;
        }
        if (digits.empty()) continue;
        // 先頭の 0 を除いた桁数が多すぎる値は扱わない (議論番号として現実的でない)。
        const auto nz = digits.find_first_not_of('0');
        if (nz == std::string::npos) return std::nullopt;  // 0
        if (digits.size() - nz > 15) return std::nullopt;
        const auto n = str::parse_int(digits.substr(nz));
        if (!n || *n <= 0) return std::nullopt;
        return n;
    }
    return std::nullopt;
}

std::optional<ReusablePaper> load_reusable_paper(std::int64_t discussion_no) {
    auto row = db::get_flow_db()->get(
        R"SQL(SELECT session_id, theme, tags_json, mechanics_json, supplement, body_md
         FROM discussion_paper WHERE discussion_no = ? LIMIT 1)SQL",
        {discussion_no});
    if (!row) return std::nullopt;
    ReusablePaper r;
    r.discussionNo = discussion_no;
    r.sessionId = row->str("session_id");
    r.theme = row->str("theme");
    if (auto v = parse_json(row->str("mechanics_json", "[]"), JsonLimits{16u << 20, 1'000'000, 64}); v && v->is_array())
        for (const auto& m : *v) r.mechanics.push_back(mechanic_summary_from_json(m));
    const auto body = row->opt_str("body_md");
    if (body && !body->empty()) {
        PaperContent fallback;
        fallback.theme = r.theme;
        fallback.supplement = row->str("supplement");
        fallback.mechanics = r.mechanics;
        r.mechanicsContext = paper_fixed_fields_from_markdown(strip_progress(*body), fallback).mechanicsContext;
    }
    r.voices = load_paper_voices(r.sessionId);
    return r;
}

std::optional<std::string> merge_reused_context(const std::optional<ReusablePaper>& reused,
                                                const std::optional<std::string>& current) {
    std::vector<std::string> unique;
    for (const std::string& part : {reused ? reused->mechanicsContext : std::string(), current.value_or("")}) {
        auto t = str::trim(part);
        if (!t.empty() && std::find(unique.begin(), unique.end(), t) == unique.end()) unique.push_back(std::move(t));
    }
    if (unique.empty()) return std::nullopt;
    return str::join(unique, "\n\n");
}

std::string describe_reuse(std::int64_t no, const std::optional<ReusablePaper>& reused) {
    const std::string n = std::to_string(no);
    if (!reused) return "♻️ ペーパー #" + n + " が見つからないため、通常どおり材料を集めます。";
    const std::string voices = !reused->voices.empty()
                                   ? "声 " + std::to_string(reused->voices.size()) + " 件 (集め直しません)"
                                   : std::string("声 (保存が無いため通常どおり集めます)");
    return "♻️ ペーパー #" + n + "「" + reused->theme + "」のメカニクス " + std::to_string(reused->mechanics.size()) +
           " 件・ゲーム内容・" + voices + "を使い回します。";
}

} // namespace di::flow
