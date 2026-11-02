#include "flow/improvement_score.hpp"

#include <limits>

#include "flow/sentiment_vector.hpp"

namespace di::flow {

Json to_json(const ScoredOpinion& s) {
    Json o = Json::object();
    o["id"] = s.id;
    o["score"] = s.score;
    o["method"] = s.method;
    if (s.detail) {
        Json arr = Json::array();
        for (const auto& c : *s.detail) arr.push_back(to_json(c));
        o["detail"] = std::move(arr);
    }
    return o;
}

double score_movement(const std::vector<double>& movement, const std::vector<double>& design_gap) {
    return scalar_projection(movement, design_gap);
}

double score_opinion(const std::vector<double>& opinion_vec, const std::vector<double>& current_vec,
                     const std::vector<double>& design_gap) {
    return score_movement(subtract(opinion_vec, current_vec), design_gap);
}

std::vector<ScoredOpinion> score_opinions(const std::vector<MovementCandidate>& candidates,
                                          const std::vector<double>& design_gap) {
    std::vector<ScoredOpinion> out;
    for (const auto& c : candidates) out.push_back({c.id, score_movement(c.movement, design_gap), c.method, c.detail});
    return out;
}

std::optional<std::string> pick_winner(const std::vector<ScoredOpinion>& scored) {
    std::optional<std::string> winner;
    double max = -std::numeric_limits<double>::infinity();
    for (const auto& s : scored) {
        if (s.score > max) {
            max = s.score;
            winner = s.id;
        }
    }
    return winner;
}

} // namespace di::flow
