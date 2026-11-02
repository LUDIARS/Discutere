#include "persona_engine/learning/learner.hpp"

#include <stdexcept>
#include <string_view>

#include "base/strings.hpp"

namespace di::persona_engine::learning {

namespace {

/** pos から JS の \s を 1 個以上読み、終わりの位置を返す (無ければ nullopt)。 */
std::optional<std::size_t> skip_spaces(std::string_view s, std::size_t pos) {
    std::size_t i = pos;
    while (i < s.size()) {
        std::size_t j = i;
        if (!str::is_js_space(str::next_codepoint(s, j))) break;
        i = j;
    }
    if (i == pos) return std::nullopt;
    return i;
}

/** pos から \S を 1 個以上読む (貪欲。後ろが \s を要求するので後戻りは結果を変えない)。 */
std::optional<std::size_t> skip_non_spaces(std::string_view s, std::size_t pos) {
    std::size_t i = pos;
    while (i < s.size()) {
        std::size_t j = i;
        if (str::is_js_space(str::next_codepoint(s, j))) break;
        i = j;
    }
    if (i == pos) return std::nullopt;
    return i;
}

} // namespace

std::optional<ProposeMatch> match_propose_pattern(const std::string& detail) {
    const std::string lower = str::to_lower_ascii(detail);
    const std::string_view s = lower;
    std::size_t from = 0;
    while (true) {
        const std::size_t start = s.find("persona", from);
        if (start == std::string_view::npos) return std::nullopt;
        from = start + 1;
        auto p = skip_spaces(s, start + 7);
        if (!p) continue;
        const std::size_t id_begin = *p;
        auto id_end = skip_non_spaces(s, id_begin);
        if (!id_end) continue;
        p = skip_spaces(s, *id_end);
        if (!p || s.substr(*p, 8) != "proposed") continue;
        p = skip_spaces(s, *p + 8);
        if (!p || s.substr(*p, 10) != "hypothesis") continue;
        p = skip_spaces(s, *p + 10);
        if (!p) continue;
        auto h_end = skip_non_spaces(s, *p);
        if (!h_end) continue;
        // 捕捉は元の文字列 (大小を保つ) から取り出す。
        return ProposeMatch{detail.substr(id_begin, *id_end - id_begin), detail.substr(*p, *h_end - *p)};
    }
}

std::string format_note(const HypothesisOutcome& outcome, const std::string& hypothesis_id,
                        const std::optional<std::string>& statement) {
    const std::string short_id = str::js_slice(hypothesis_id, 0, 8);
    const std::string stmt = statement && !statement->empty() ? " — \"" + str::js_slice(*statement, 0, 60) + "\"" : "";
    if (outcome == "integrated") return "✅ integrated: hyp:" + short_id + stmt;
    return "❌ rejected: hyp:" + short_id + stmt;
}

RecordOutcomeResult record_hypothesis_outcome(const RecordOutcomeArgs& args) {
    std::optional<std::string> proposer;
    for (const auto& log : args.rules->recent_logs(args.recentLogsLimit.value_or(500))) {
        if (!log.detail || log.detail->empty()) continue;
        const auto m = match_propose_pattern(*log.detail);
        if (m && m->hypothesisId == args.hypothesisId) {
            proposer = m->personaId;
            break;
        }
    }
    if (!proposer) return {false, std::nullopt};
    const std::string note = format_note(args.outcome, args.hypothesisId, args.statement);
    try {
        args.personas->append_learned_note(*proposer, note);
        return {true, proposer};
    } catch (const std::exception&) {
        return {false, std::nullopt};
    }
}

} // namespace di::persona_engine::learning
