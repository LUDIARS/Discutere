#include "lib/sentiment_core/speaker_profile.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "lib/sentiment_core/js_number.hpp"

namespace di::lib::sentiment_core {

namespace {

double euclidean_distance(const Vector& left, const Vector& right) {
    if (left.size() != right.size())
        throw std::invalid_argument("vector dim mismatch: " + std::to_string(left.size()) + " vs " +
                                    std::to_string(right.size()));
    double sum = 0;
    for (std::size_t i = 0; i < left.size(); ++i) sum += (left[i] - right[i]) * (left[i] - right[i]);
    return std::sqrt(sum);
}

struct GameAggregate {
    std::string key;
    std::vector<std::string> texts;
    double weight = 0;
};

} // namespace

SpeakerEvaluation evaluate_speakers(const std::vector<SpeakerOpinions>& speakers, const EvaluateSpeakerOptions& options) {
    const double min_opinions = options.minOpinions.value_or(10);
    const double polarity_eps = options.polarityEps.value_or(0.05);
    const double game_gap_eps = options.gameGapEps.value_or(0.05);
    SpeakerEvaluation out;
    for (const auto& speaker : speakers) {
        const double count = static_cast<double>(speaker.opinions.size());
        if (count < min_opinions) {
            out.rejected.push_back({speaker.speakerId, "too-few", count});
            continue;
        }
        // Map の挿入順でゲームごとに束ねる
        std::vector<GameAggregate> games;
        for (const auto& opinion : speaker.opinions) {
            const std::string key = opinion.gameSlug.value_or("_none");
            auto it = std::find_if(games.begin(), games.end(), [&](const GameAggregate& g) { return g.key == key; });
            if (it == games.end()) {
                games.push_back({key, {}, 0});
                it = games.end() - 1;
            }
            it->texts.push_back(opinion.text);
            it->weight += opinion.weight.value_or(1);
        }
        if (games.size() < 2) {
            out.rejected.push_back({speaker.speakerId, "no-game-gap", count});
            continue;
        }
        std::vector<Vector> vectors;
        std::vector<double> weights;
        for (const auto& g : games) {
            std::string joined;
            for (std::size_t i = 0; i < g.texts.size(); ++i) {
                if (i) joined += " \n ";
                joined += g.texts[i];
            }
            vectors.push_back(text_to_vector(joined));
            weights.push_back(g.weight);
        }
        double positive = 0, negative = 0;
        for (const auto& v : vectors) {
            if (v[0] > 0.5 + polarity_eps) positive += 1;
            if (v[0] < 0.5 - polarity_eps) negative += 1;
        }
        const double n = static_cast<double>(vectors.size());
        if (positive == n) {
            out.rejected.push_back({speaker.speakerId, "all-positive", count});
            continue;
        }
        if (negative == n) {
            out.rejected.push_back({speaker.speakerId, "all-negative", count});
            continue;
        }
        double maximum_gap = 0, pair_distance_sum = 0, pair_count = 0;
        for (std::size_t l = 0; l < vectors.size(); ++l) {
            for (std::size_t r = l + 1; r < vectors.size(); ++r) {
                const double d = euclidean_distance(vectors[l], vectors[r]);
                maximum_gap = std::max(maximum_gap, d);
                pair_distance_sum += d;
                pair_count += 1;
            }
        }
        if (maximum_gap <= game_gap_eps) {
            out.rejected.push_back({speaker.speakerId, "no-game-gap", count});
            continue;
        }
        Vector affect = weighted_mean(vectors, weights);
        if (affect.size() != DIM) throw std::runtime_error("affect vector dim must be " + std::to_string(DIM));
        SpeakerCandidate c;
        c.speakerId = speaker.speakerId;
        c.source = speaker.source;
        c.affect = std::move(affect);
        c.count = count;
        c.polarityBias = js_round_fixed(std::fabs(positive - negative) / n, 4);
        c.affectDispersion = pair_count != 0 ? js_round_fixed(pair_distance_sum / pair_count, 4) : 0;
        out.candidates.push_back(std::move(c));
    }
    return out;
}

Json to_json(const SpeakerEvaluation& evaluation) {
    Json candidates = Json::array();
    for (const auto& c : evaluation.candidates) {
        Json o = Json::object();
        o["speakerId"] = c.speakerId;
        if (c.source) o["source"] = *c.source;  // undefined は JSON.stringify で消える
        o["affect"] = vector_to_json(c.affect);
        o["count"] = c.count;
        o["polarityBias"] = c.polarityBias;
        o["affectDispersion"] = c.affectDispersion;
        candidates.push_back(std::move(o));
    }
    Json rejected = Json::array();
    for (const auto& r : evaluation.rejected) {
        Json o = Json::object();
        o["speakerId"] = r.speakerId;
        o["reason"] = r.reason;
        o["count"] = r.count;
        rejected.push_back(std::move(o));
    }
    Json out = Json::object();
    out["candidates"] = std::move(candidates);
    out["rejected"] = std::move(rejected);
    return out;
}

} // namespace di::lib::sentiment_core
