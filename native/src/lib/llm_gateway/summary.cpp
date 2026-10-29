#include "lib/llm_gateway/summary.hpp"

#include <algorithm>

#include "lib/llm_gateway/tokens.hpp"

namespace di::lib::llm_gateway {

RollingSummaryResult rolling_summary(const std::vector<HistoryTurn>& turns, const Summarizer& summarize,
                                     const RollingSummaryOptions& opts) {
    auto est = [&](std::string_view t) -> double {
        return opts.estimateTokens ? opts.estimateTokens(t) : static_cast<double>(estimate_tokens(t));
    };
    double total = 0;
    for (const auto& t : turns) total += est(t.content);
    RollingSummaryResult result;
    if (total <= opts.maxTokens) {
        result.recent = turns;
        return result;
    }
    const std::size_t keep = static_cast<std::size_t>(std::max<long long>(0, opts.keepRecent));
    // turns.slice(-keep) / turns.slice(0, -keep)
    const std::size_t split = keep >= turns.size() ? 0 : turns.size() - keep;
    std::vector<HistoryTurn> older;
    if (keep > 0) {
        result.recent.assign(turns.begin() + static_cast<std::ptrdiff_t>(split), turns.end());
        older.assign(turns.begin(), turns.begin() + static_cast<std::ptrdiff_t>(split));
    } else {
        older = turns;
    }
    // 畳む対象が無い (履歴が keepRecent 以下) なら要約しようがない。
    if (older.empty()) return result;
    result.summary = summarize(older);
    result.compacted = true;
    return result;
}

} // namespace di::lib::llm_gateway
