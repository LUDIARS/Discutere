#pragma once
// Persona 別の意見・合意スコアの集計 (副作用なしの変換。src/api/consensus-persona-aggregation.ts の移植)。
// @implements SPEC-THALEIA-PERSONA-CONSENSUS
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::api {

struct PersonaConsensusUtterance {
    std::string id;
    std::string content;
    double postedAt = 0;
    std::optional<bool> agree;
    std::optional<double> score;
    std::optional<std::string> reasoning;
};
Json to_json(const PersonaConsensusUtterance& u);

struct PersonaConsensusSource : PersonaConsensusUtterance {
    double opinionScore = 0;
    std::string personaId;
    std::string displayName;
    std::vector<std::string> traits;
};

struct PersonaConsensusSummary {
    std::string personaId;
    std::string displayName;
    std::vector<std::string> traits;
    std::vector<PersonaConsensusUtterance> utterances;
    double opinionScore = 0;
    double consensusScore = 0;
    double agreeRatio = 0;
};
Json to_json(const PersonaConsensusSummary& s);

/** personaId ごとにまとめ、発話は postedAt 昇順、persona は displayName の localeCompare 順 (安定)。 */
std::vector<PersonaConsensusSummary> aggregate_persona_consensus(const std::vector<PersonaConsensusSource>& rows);

/** 未採点 (agree が null) の発話は分母に含めない。採点済みが無ければ 0。 */
double ratio_of_agreed(const std::vector<PersonaConsensusUtterance>& utterances);

} // namespace di::api
