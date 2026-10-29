#include "lib/llm_gateway/ordering.hpp"

#include <algorithm>

#include "base/strings.hpp"

namespace di::lib::llm_gateway {

namespace {
int rank(const Stability& s) {
    if (s == "fixed") return 0;
    if (s == "semi") return 1;
    if (s == "volatile") return 2;
    // TS では未知の値は RANK[x] = undefined で比較が NaN になり順序が不定。ここでは末尾に寄せる。
    return 3;
}
} // namespace

OrderedPrompt order_segments(const std::vector<Segment>& segments) {
    std::vector<const Segment*> ordered;
    ordered.reserve(segments.size());
    for (const auto& s : segments)
        if (!str::trim(s.text).empty()) ordered.push_back(&s);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Segment* a, const Segment* b) { return rank(a->stability) < rank(b->stability); });

    OrderedPrompt out;
    std::vector<bool> stable_flags;
    for (const Segment* seg : ordered) {
        const bool stable = seg->stability != "volatile";
        if (!out.messages.empty() && out.messages.back().role == seg->role && stable_flags.back() == stable) {
            out.messages.back().content += "\n\n" + seg->text;
        } else {
            out.messages.push_back({seg->role, seg->text});
            stable_flags.push_back(stable);
        }
    }
    for (bool flag : stable_flags) {
        if (!flag) break;
        ++out.cacheBreakpoint;
    }
    return out;
}

} // namespace di::lib::llm_gateway
