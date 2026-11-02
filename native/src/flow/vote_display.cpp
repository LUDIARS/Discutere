#include "flow/vote_display.hpp"

#include <algorithm>

#include "base/json.hpp"
#include "lib/sentiment_core/js_number.hpp"

namespace di::flow {

bool should_stack_vote_reactions(const std::string& kind) { return kind != "scores"; }

bool has_visible_vote_result(const VoteDisplay& e) {
    if (e.kind == "scores") return e.winner.has_value();
    double sum = 0;
    for (const auto& [id, n] : e.tally) sum += n;
    return sum > 0;
}

std::vector<std::string> build_vote_ranking_lines(const VoteDisplay& e,
                                                  const std::function<std::string(const std::string&)>& name_of) {
    const bool is_scores = e.kind == "scores";
    std::vector<std::pair<std::string, double>> rows;
    for (const auto& row : e.tally)
        if (is_scores || row.second > 0) rows.push_back(row);
    // Array#sort は安定 (同点は元の順)。
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return b.second < a.second; });
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& [id, n] = rows[i];
        const std::string name = name_of ? name_of(id) : id;
        const std::string medal = e.winner && id == *e.winner ? std::string("🏆") : std::to_string(i + 1) + ".";
        const std::string value = is_scores ? "スコア " + lib::sentiment_core::js_to_fixed(n, 2) + " (design_gap 適合)"
                                            : js_number_to_string(n) + "票";
        out.push_back(medal + " " + name + " — " + value);
    }
    return out;
}

std::string vote_notice_title(int round, const std::string& kind) {
    return kind == "scores" ? "📐 ラウンド " + std::to_string(round) + " 機械スコア (design_gap 適合)"
                            : "🗳️ ラウンド " + std::to_string(round) + " 投票結果";
}

} // namespace di::flow
