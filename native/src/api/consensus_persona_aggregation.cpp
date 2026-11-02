#include "api/consensus_persona_aggregation.hpp"

#include <algorithm>
#include <map>

#include "visualize/locale_compare.hpp"

namespace di::api {

Json to_json(const PersonaConsensusUtterance& u) {
    Json j = Json::object();
    j["id"] = u.id;
    j["content"] = u.content;
    j["postedAt"] = u.postedAt;
    j["agree"] = u.agree ? Json(*u.agree) : Json(nullptr);
    j["score"] = u.score ? Json(*u.score) : Json(nullptr);
    j["reasoning"] = u.reasoning ? Json(*u.reasoning) : Json(nullptr);
    return j;
}

Json to_json(const PersonaConsensusSummary& s) {
    Json utterances = Json::array();
    for (const auto& u : s.utterances) utterances.push_back(to_json(u));
    Json j = Json::object();
    j["personaId"] = s.personaId;
    j["displayName"] = s.displayName;
    j["traits"] = s.traits;
    j["utterances"] = utterances;
    j["opinionScore"] = s.opinionScore;
    j["consensusScore"] = s.consensusScore;
    j["agreeRatio"] = s.agreeRatio;
    return j;
}

std::vector<PersonaConsensusSummary> aggregate_persona_consensus(const std::vector<PersonaConsensusSource>& rows) {
    std::vector<PersonaConsensusSummary> grouped;  // Map の挿入順
    std::map<std::string, std::size_t> index;
    for (const auto& row : rows) {
        auto it = index.find(row.personaId);
        if (it == index.end()) {
            PersonaConsensusSummary s;
            s.personaId = row.personaId;
            s.displayName = row.displayName;
            s.traits = row.traits;
            it = index.emplace(row.personaId, grouped.size()).first;
            grouped.push_back(std::move(s));
        }
        auto& current = grouped[it->second];
        current.utterances.push_back(static_cast<const PersonaConsensusUtterance&>(row));
        current.opinionScore += row.opinionScore;
        current.consensusScore += row.score.value_or(0);
    }
    for (auto& persona : grouped) {
        persona.agreeRatio = ratio_of_agreed(persona.utterances);
        std::stable_sort(persona.utterances.begin(), persona.utterances.end(),
                         [](const auto& a, const auto& b) { return a.postedAt < b.postedAt; });
    }
    std::stable_sort(grouped.begin(), grouped.end(), [](const auto& a, const auto& b) {
        return visualize::locale_compare(a.displayName, b.displayName) < 0;
    });
    return grouped;
}

double ratio_of_agreed(const std::vector<PersonaConsensusUtterance>& utterances) {
    std::size_t scored = 0;
    std::size_t agreed = 0;
    for (const auto& u : utterances) {
        if (!u.agree) continue;
        ++scored;
        if (*u.agree) ++agreed;
    }
    if (scored == 0) return 0;
    return static_cast<double>(agreed) / static_cast<double>(scored);
}

} // namespace di::api
