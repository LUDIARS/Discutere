#include "lib/sentiment_core/affect_score.hpp"

#include "lib/sentiment_core/vector.hpp"

namespace di::lib::sentiment_core {

const std::vector<std::string>& emotions() { return emo_keys(); }
const std::vector<std::string>& aspects() { return asp_keys(); }

AffectScore score_text(std::string_view text) {
    const TextFeatures f = extract_text_features(text);
    AffectScore s;
    s.valence = f.valence;
    s.valenceLabel = f.valence > 0.05 ? "positive" : f.valence < -0.05 ? "negative" : "neutral";
    const auto& emo = emo_keys();
    for (std::size_t i = 0; i < emo.size(); ++i)
        if (f.emotions[i] > 0) s.emotions.push_back(emo[i]);
    const auto& asp = asp_keys();
    for (std::size_t i = 0; i < asp.size(); ++i) {
        s.aspects.emplace_back(asp[i], f.aspectMentions[i]);
        if (f.aspectMentions[i] > 0) s.aspectsHit.push_back(asp[i]);
    }
    s.arousal = f.arousal;
    return s;
}

Json to_json(const AffectScore& score) {
    Json aspects_json = Json::object();
    for (const auto& [k, v] : score.aspects) aspects_json[k] = v;
    Json out = Json::object();
    out["valence"] = score.valence;
    out["valenceLabel"] = score.valenceLabel;
    out["emotions"] = score.emotions;
    out["aspects"] = std::move(aspects_json);
    out["aspectsHit"] = score.aspectsHit;
    out["arousal"] = score.arousal;
    return out;
}

} // namespace di::lib::sentiment_core
