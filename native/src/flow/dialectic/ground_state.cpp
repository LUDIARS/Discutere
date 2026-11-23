#include "flow/dialectic/ground_state.hpp"

namespace di::flow::dialectic {

std::string ground_state_label(const GroundState& s) {
    if (s == GROUND_UNCHALLENGED) return "反論なし";
    if (s == GROUND_DEFENDED) return "守った";
    if (s == GROUND_UNSETTLED) return "未決";
    if (s == GROUND_CHALLENGED) return "崩れた";
    if (s == GROUND_CONCEDED) return "認めた";
    return s;
}

GroundTally tally_grounds(const std::vector<Ground>& grounds) {
    GroundTally t;
    for (const auto& g : grounds) {
        ++t.total;
        if (g.state == GROUND_UNCHALLENGED) ++t.unchallenged;
        else if (g.state == GROUND_DEFENDED) ++t.defended;
        else if (is_unsettled_ground(g.state)) ++t.unsettled;
        else if (is_broken_ground(g.state)) ++t.broken;
    }
    return t;
}

std::string render_ground_tally(const GroundTally& t) {
    return "反論なし " + std::to_string(t.unchallenged) + " / 守った " + std::to_string(t.defended) + " / 未決 " +
           std::to_string(t.unsettled) + " / 崩れた " + std::to_string(t.broken);
}

std::vector<std::string> ground_ids_where(const std::vector<Ground>& grounds, bool (*pred)(const GroundState&)) {
    std::vector<std::string> out;
    for (const auto& g : grounds)
        if (pred(g.state)) out.push_back(g.id);
    return out;
}

} // namespace di::flow::dialectic
