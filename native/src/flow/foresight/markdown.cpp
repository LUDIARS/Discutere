#include "flow/foresight/markdown.hpp"

#include "base/strings.hpp"

namespace di::flow::foresight {
namespace {

constexpr std::string_view kHeadForesight = "# 未来予測アドバイザリー (参考)";
constexpr std::string_view kHeadBase = "# 未来予測";
constexpr std::string_view kAdvisory = "アドバイザリー (参考)";

/** 1 行が /^# 未来予測(?: ?アドバイザリー \(参考\))?[ \t]*$/ に一致するか。 */
bool is_strip_head(std::string_view line) {
    if (!str::starts_with(line, kHeadBase)) return false;
    std::string_view rest = line.substr(kHeadBase.size());
    auto only_blank = [](std::string_view s) {
        for (char c : s)
            if (c != ' ' && c != '\t') return false;
        return true;
    };
    if (only_blank(rest)) return true;
    std::string_view r = rest;
    if (!r.empty() && r[0] == ' ') r.remove_prefix(1);
    if (str::starts_with(r, kAdvisory)) return only_blank(r.substr(kAdvisory.size()));
    return false;
}

} // namespace

namespace {

/** JS の /m で行の境目になる文字 (\n \r U+2028 U+2029) の位置と長さ。無ければ npos。 */
std::pair<std::size_t, std::size_t> next_line_break(std::string_view s, std::size_t from) {
    for (std::size_t i = from; i < s.size(); ++i) {
        if (s[i] == '\n' || s[i] == '\r') return {i, 1};
        // U+2028 / U+2029 は UTF-8 で E2 80 A8 / E2 80 A9
        if (static_cast<unsigned char>(s[i]) == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(s[i + 2]) == 0xA8 || static_cast<unsigned char>(s[i + 2]) == 0xA9))
            return {i, 3};
    }
    return {std::string_view::npos, 0};
}

} // namespace

std::string strip_foresight(std::string_view body_md) {
    // /m の ^ と $ は行の境目 (\n \r U+2028 U+2029) の前後で一致する。最初に一致した見出しから後ろを捨てる。
    std::size_t start = 0;
    std::string_view base = body_md;
    while (true) {
        const auto [brk, len] = next_line_break(body_md, start);
        const std::size_t end = brk == std::string_view::npos ? body_md.size() : brk;
        if (is_strip_head(body_md.substr(start, end - start))) {
            base = body_md.substr(0, start);
            break;
        }
        if (brk == std::string_view::npos) break;
        start = brk + len;
    }
    return str::trim_end(base);
}


std::string append_foresight_markdown(std::string_view body_md, const ForesightSection& section) {
    const std::string proposal = str::trim(section.proposal);
    const std::string forecast = str::trim(section.forecast);
    std::string foresight;
    foresight += kHeadForesight;
    foresight += "\n\n結論とは別の参考です。結論は上の「# 結論」を正とします。\n\n";
    foresight += "## 破壊的な一手の提案\n" + (proposal.empty() ? std::string("(提案なし)") : proposal) + "\n\n";
    foresight += "## 未解決の前提と 3 / 5 / 10 年時点のシナリオ\n" + (forecast.empty() ? std::string("(予測なし)") : forecast);
    const std::string base = strip_foresight(body_md);
    return base.empty() ? foresight : base + "\n\n" + foresight;
}

} // namespace di::flow::foresight
