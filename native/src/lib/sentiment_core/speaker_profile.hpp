#pragma once
// @ludiars/sentiment-core speaker-profile.ts の移植: 話者ごとのゲーム別ベクトルから人物像の候補を選ぶ。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "lib/sentiment_core/vector.hpp"

namespace di::lib::sentiment_core {

struct SpeakerOpinion {
    std::string text;
    std::optional<std::string> gameSlug;  // null 可
    std::optional<double> weight;
};

struct SpeakerOpinions {
    std::string speakerId;
    std::optional<std::string> source;
    std::vector<SpeakerOpinion> opinions;
};

struct EvaluateSpeakerOptions {
    std::optional<double> minOpinions;
    std::optional<double> polarityEps;
    std::optional<double> gameGapEps;
};

/** "too-few" | "all-positive" | "all-negative" | "no-game-gap" */
using SpeakerRejectReason = std::string;

struct SpeakerCandidate {
    std::string speakerId;
    std::optional<std::string> source;
    Vector affect;
    double count = 0;
    double polarityBias = 0;
    double affectDispersion = 0;
};

struct SpeakerRejected {
    std::string speakerId;
    SpeakerRejectReason reason;
    double count = 0;
};

struct SpeakerEvaluation {
    std::vector<SpeakerCandidate> candidates;
    std::vector<SpeakerRejected> rejected;
};

SpeakerEvaluation evaluate_speakers(const std::vector<SpeakerOpinions>& speakers, const EvaluateSpeakerOptions& options = {});

Json to_json(const SpeakerEvaluation& evaluation);

} // namespace di::lib::sentiment_core
