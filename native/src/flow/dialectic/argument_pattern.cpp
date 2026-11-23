#include "flow/dialectic/argument_pattern.hpp"

#include <cstdint>
#include <set>

#include "base/strings.hpp"

namespace di::flow::dialectic {

namespace {

std::uint32_t fnv1a(const std::string& s) {
    std::uint32_t h = 2166136261u;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

bool is_patterned(const FlowPersona& p) { return p.role != "facilitator" && !p.proxy; }

/** start から順に、used に無い最初の index (全部使っていれば start)。 */
std::size_t pick_unused(std::size_t start, std::size_t n, const std::set<std::size_t>& used) {
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t i = (start + k) % n;
        if (!used.contains(i)) return i;
    }
    return start % n;
}

template <class T>
std::optional<std::size_t> index_of(const std::vector<T>& v, const std::optional<std::string>& id) {
    if (!id) return std::nullopt;
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].id == *id) return i;
    return std::nullopt;
}

} // namespace

std::map<std::string, ArgumentPatternOverride> parse_argument_pattern_overrides(const std::string& paper_md,
                                                                                 const ArgumentPatternCatalog& catalog) {
    std::map<std::string, ArgumentPatternOverride> out;
    bool in_section = false;
    for (const auto& raw : str::split_lines(paper_md)) {
        const std::string line = str::trim(raw);
        if (line.rfind("# ", 0) == 0) {
            in_section = str::trim(line.substr(2)) == "論じ方の癖";
            continue;
        }
        if (!in_section || line.rfind("- ", 0) != 0) continue;
        const std::string body = str::trim(line.substr(2));
        auto colon = body.find("：");
        std::size_t colon_len = 3;
        if (colon == std::string::npos) {
            colon = body.find(':');
            colon_len = 1;
        }
        if (colon == std::string::npos) continue;
        const std::string name = str::trim(body.substr(0, colon));
        if (name.empty()) continue;
        ArgumentPatternOverride o;
        for (const auto& part : str::split(body.substr(colon + colon_len), "/")) {
            const std::string v = str::trim(part);
            if (const auto* sp = find_script_pattern(catalog, v)) o.scriptPattern = sp->id;
            else if (const auto* rs = find_reasoning_style(catalog, v)) o.reasoningStyle = rs->id;
        }
        if (o.scriptPattern || o.reasoningStyle) out[name] = o;
    }
    return out;
}

void assign_argument_patterns(std::vector<FlowPersona>& cast, const std::string& seed,
                              const std::map<std::string, ArgumentPatternOverride>& overrides, const ArgumentPatternCatalog& catalog) {
    const std::size_t np = catalog.scriptPatterns.size();
    const std::size_t nr = catalog.reasoningStyles.size();
    if (np == 0 || nr == 0) return;
    std::map<std::string, std::set<std::size_t>> used_p;
    std::map<std::string, std::set<std::size_t>> used_r;
    // 1) 人の上書きを先に置く (陣営の使用済みに数える)。
    for (auto& p : cast) {
        if (!is_patterned(p)) continue;
        const auto it = overrides.find(p.name);
        if (it == overrides.end()) continue;
        if (it->second.scriptPattern) p.scriptPattern = it->second.scriptPattern;
        if (it->second.reasoningStyle) p.reasoningStyle = it->second.reasoningStyle;
        if (auto i = index_of(catalog.scriptPatterns, p.scriptPattern)) used_p[p.stance].insert(*i);
        if (auto i = index_of(catalog.reasoningStyles, p.reasoningStyle)) used_r[p.stance].insert(*i);
    }
    // 2) 残りは seed と陣営から決まる位置から、陣営で未使用のものを順に取る (cast の並び順で決定的)。
    std::map<std::string, std::size_t> order;
    for (auto& p : cast) {
        if (!is_patterned(p)) continue;
        const std::size_t k = order[p.stance]++;
        const std::uint32_t h = fnv1a(seed + "\n" + p.stance);
        if (!index_of(catalog.scriptPatterns, p.scriptPattern)) {
            const std::size_t i = pick_unused(h % np + k, np, used_p[p.stance]);
            used_p[p.stance].insert(i);
            p.scriptPattern = catalog.scriptPatterns[i].id;
        }
        if (!index_of(catalog.reasoningStyles, p.reasoningStyle)) {
            const std::size_t i = pick_unused((h / 7) % nr + k, nr, used_r[p.stance]);
            used_r[p.stance].insert(i);
            p.reasoningStyle = catalog.reasoningStyles[i].id;
        }
    }
}

std::vector<std::string> argument_pattern_lines(const FlowPersona& persona, ArgumentPhase phase, bool pressed,
                                                const ArgumentPatternCatalog& catalog) {
    std::vector<std::string> lines;
    if (!is_patterned(persona)) return lines;
    const ScriptPattern* sp = persona.scriptPattern ? find_script_pattern(catalog, *persona.scriptPattern) : nullptr;
    const ReasoningStyle* rs = persona.reasoningStyle ? find_reasoning_style(catalog, *persona.reasoningStyle) : nullptr;
    if (sp) {
        const std::string& habit = phase == ArgumentPhase::position ? sp->position : pressed ? sp->stress : sp->exchange;
        lines.push_back("あなたの論じ方の癖: " + habit +
                        " (癖の名前は名乗らない。データと論拠は分けて書き、人を否定せず論拠に反論する)");
    }
    if (rs) lines.push_back(rs->line);
    return lines;
}

} // namespace di::flow::dialectic
