#include "discatier_engine_adapter/game_aliases.hpp"

#include <algorithm>
#include <unordered_set>

#include "base/strings.hpp"

namespace di::discatier_engine_adapter {
namespace {

bool is_open(std::uint32_t c) { return c == '(' || c == 0xFF08; }   // ( （
bool is_close(std::uint32_t c) { return c == ')' || c == 0xFF09; }  // ) ）

using LowerSet = std::unordered_set<std::string>;

/** 2 グループが (lower-case で) 共通要素を持つか。 */
bool shares(const LowerSet& a, const std::vector<std::string>& b) {
    return std::any_of(b.begin(), b.end(), [&](const std::string& x) { return a.count(str::to_lower(x)) > 0; });
}

/** 語 t が別名グループ member と関連するか (完全一致 or 4 文字以上での部分一致)。 */
bool relates(const std::string& t, const std::string& member) {
    if (t == member) return true;
    if (str::js_length(member) >= 4 && str::contains(t, member)) return true;
    if (str::js_length(t) >= 4 && str::contains(member, t)) return true;
    return false;
}

} // namespace

const std::vector<std::vector<std::string>>& static_alias_groups() {
    static const std::vector<std::vector<std::string>> groups = {
        {"モンスターストライク", "モンスト"},
        {"death stranding", "デス・ストランディング", "デスストランディング", "デススト"},
        {"vampire survivors", "ヴァンパイアサバイバーズ", "ヴァンサバ"},
        {"hollow knight", "ホロウナイト", "ホロナイ"},
        {"genshin impact", "原神", "genshin"},
        {"shadow of the colossus", "ワンダと巨像", "ワンダ"},
    };
    return groups;
}

std::vector<std::string> parse_title_aliases(const std::string& title) {
    std::vector<std::string> out;
    const auto cps = str::codepoints(title);
    // title.split(/[（(]/)[0].trim()
    std::size_t first_open = 0;
    while (first_open < cps.size() && !is_open(cps[first_open])) ++first_open;
    const std::string outer = str::trim(str::from_codepoints({cps.begin(), cps.begin() + static_cast<std::ptrdiff_t>(first_open)}));
    if (str::js_length(outer) >= 2) out.push_back(outer);
    // title.match(/[（(]([^）)]+)[）)]/) — 最左の一致
    for (std::size_t p = first_open; p < cps.size(); ++p) {
        if (!is_open(cps[p])) continue;
        std::size_t j = p + 1;
        while (j < cps.size() && !is_close(cps[j])) ++j;
        if (j == cps.size()) break;   // 閉じ括弧が無い → 以降の開き括弧でも一致しない
        if (j == p + 1) continue;     // 中身が空
        const std::string inner = str::trim(str::from_codepoints({cps.begin() + static_cast<std::ptrdiff_t>(p + 1),
                                                                  cps.begin() + static_cast<std::ptrdiff_t>(j)}));
        if (str::js_length(inner) >= 2) out.push_back(inner);
        break;
    }
    return out;
}

std::vector<std::vector<std::string>> build_alias_groups(const std::vector<std::string>& titles,
                                                         const std::vector<std::vector<std::string>>& extra_groups) {
    std::vector<std::vector<std::string>> groups;
    std::vector<LowerSet> lower_sets;
    auto add_group = [&](const std::vector<std::string>& members) {
        std::vector<std::string> cleaned;
        for (const auto& m : members) {
            std::string t = str::trim(m);
            if (str::js_length(t) >= 2) cleaned.push_back(std::move(t));
        }
        if (cleaned.empty()) return;
        std::vector<std::string> lower;
        for (const auto& m : cleaned) lower.push_back(str::to_lower(m));
        // 既存グループと共通要素があればマージ。
        std::size_t idx = 0;
        while (idx < lower_sets.size() && !shares(lower_sets[idx], cleaned)) ++idx;
        if (idx < lower_sets.size()) {
            for (std::size_t i = 0; i < cleaned.size(); ++i) {
                if (!lower_sets[idx].count(lower[i])) {
                    groups[idx].push_back(cleaned[i]);
                    lower_sets[idx].insert(lower[i]);
                }
            }
        } else {
            groups.push_back(cleaned);
            lower_sets.emplace_back(lower.begin(), lower.end());
        }
    };
    for (const auto& title : titles) add_group(parse_title_aliases(title));
    for (const auto& g : static_alias_groups()) add_group(g);
    for (const auto& g : extra_groups) add_group(g);
    // 単一要素グループ (別名が無い) は展開に寄与しないので落とす。
    std::vector<std::vector<std::string>> out;
    for (auto& g : groups)
        if (g.size() >= 2) out.push_back(std::move(g));
    return out;
}

std::vector<std::string> expand_aliases(const std::vector<std::string>& terms,
                                        const std::vector<std::vector<std::string>>& groups, std::size_t max) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    auto add = [&](std::string s) {
        if (seen.insert(s).second) out.push_back(std::move(s));
    };
    for (const auto& raw : terms) {
        const std::string t = str::to_lower(str::trim(raw));
        if (t.empty()) continue;
        add(t);
        for (const auto& g : groups) {
            const bool hit = std::any_of(g.begin(), g.end(), [&](const std::string& m) { return relates(t, str::to_lower(m)); });
            if (hit)
                for (const auto& m : g) add(str::to_lower(m));
        }
    }
    if (out.size() > max) out.resize(max);
    return out;
}

} // namespace di::discatier_engine_adapter
