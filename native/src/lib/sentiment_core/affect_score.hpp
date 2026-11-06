#pragma once
// @ludiars/sentiment-core affect-score.ts の移植。
#include <string>
#include <utility>
#include <vector>

#include "base/json.hpp"

namespace di::lib::sentiment_core {

/** EMOTIONS / ASPECTS (= EMO_KEYS / ASP_KEYS)。 */
const std::vector<std::string>& emotions();
const std::vector<std::string>& aspects();

/** "positive" | "negative" | "neutral" */
using ValenceLabel = std::string;

struct AffectScore {
    double valence = 0;
    ValenceLabel valenceLabel;
    std::vector<std::string> emotions;
    /** アスペクト → 一致数 (ASPECTS の順)。 */
    std::vector<std::pair<std::string, double>> aspects;
    std::vector<std::string> aspectsHit;
    double arousal = 0;
};

AffectScore score_text(std::string_view text);
Json to_json(const AffectScore& score);

} // namespace di::lib::sentiment_core
