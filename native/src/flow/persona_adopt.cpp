#include "flow/persona_adopt.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "crawler/sources/persona.hpp"
#include "flow/sentiment_vector.hpp"
#include "lib/sentiment_core/js_number.hpp"

namespace di::flow {
namespace {

using lib::sentiment_core::js_round_fixed;

std::vector<double> mean_of(const std::vector<std::vector<double>>& vectors) {
    std::vector<double> out(DIM, 0.0);
    if (vectors.empty()) return out;
    for (const auto& v : vectors)
        for (std::size_t d = 0; d < DIM; ++d) out[d] += d < v.size() ? v[d] : std::nan("");
    for (auto& x : out) x /= static_cast<double>(vectors.size());
    return out;
}

std::vector<double> weighted_mean4(const std::vector<std::vector<double>>& vectors, const std::vector<double>& weights) {
    double wsum = 0;
    for (double w : weights) wsum += w;
    if (wsum == 0 || std::isnan(wsum)) wsum = 1;  // `|| 1`
    std::vector<double> out(DIM, 0.0);
    for (std::size_t i = 0; i < vectors.size(); ++i)
        for (std::size_t d = 0; d < DIM; ++d) out[d] += (vectors[i][d] * weights[i]) / wsum;
    for (auto& x : out) x = js_round_fixed(x, 4);
    return out;
}

double euclid(const std::vector<double>& a, const std::vector<double>& b) {
    double s = 0;
    for (std::size_t i = 0; i < a.size(); ++i) s += std::pow(a[i] - (i < b.size() ? b[i] : std::nan("")), 2);
    return std::sqrt(s);
}

} // namespace

Json to_json(const AdoptCandidate& c) {
    Json o = Json::object();
    o["speakerId"] = c.speakerId;
    if (c.source) o["source"] = *c.source;
    o["affect"] = Json::array();
    for (double x : c.affect) o["affect"].push_back(x);
    o["count"] = c.count;
    o["polarityBias"] = c.polarityBias;
    o["affectDispersion"] = c.affectDispersion;
    return o;
}

Json to_json(const AdoptRejection& r) { return Json{{"speakerId", r.speakerId}, {"reason", r.reason}, {"count", r.count}}; }

EvaluateSpeakersResult evaluate_speakers(const std::vector<SpeakerOpinions>& speakers, const AdoptOptions& opts) {
    const double min_opinions = opts.minOpinions.value_or(10);
    const double eps = opts.polarityEps.value_or(0.05);
    const double gap_eps = opts.gameGapEps.value_or(0.05);

    EvaluateSpeakersResult out;
    for (const auto& sp : speakers) {
        const double count = static_cast<double>(sp.opinions.size());
        if (count < min_opinions) {
            out.rejected.push_back({sp.speakerId, "too-few", count});
            continue;
        }
        // (b) ゲーム単位で本文連結 + (a) エンゲージメント (opinion-score = いいね) を合算。挿入順 (Map) を保つ。
        struct Game {
            std::vector<std::string> texts;
            double weight = 0;
        };
        std::vector<std::pair<std::string, Game>> by_game;
        for (const auto& o : sp.opinions) {
            const std::string key = o.gameSlug ? *o.gameSlug : "_none";
            auto it = std::find_if(by_game.begin(), by_game.end(), [&](const auto& g) { return g.first == key; });
            if (it == by_game.end()) {
                by_game.push_back({key, {}});
                it = by_game.end() - 1;
            }
            it->second.texts.push_back(o.text);
            it->second.weight += o.weight.value_or(1);
        }
        // ゲームを区別している = ≥2 ゲーム必須。
        if (by_game.size() < 2) {
            out.rejected.push_back({sp.speakerId, "no-game-gap", count});
            continue;
        }
        std::vector<std::vector<double>> game_vecs;
        std::vector<double> game_weights;
        for (const auto& g : by_game) {
            game_vecs.push_back(text_to_vector(str::join(g.second.texts, " \n ")));
            game_weights.push_back(g.second.weight);
        }
        const double total = static_cast<double>(game_vecs.size());
        double pos = 0, neg = 0;
        for (const auto& v : game_vecs) {
            if (v[0] > 0.5 + eps) ++pos;
            if (v[0] < 0.5 - eps) ++neg;
        }
        if (pos == total) {
            out.rejected.push_back({sp.speakerId, "all-positive", count});
            continue;
        }
        if (neg == total) {
            out.rejected.push_back({sp.speakerId, "all-negative", count});
            continue;
        }
        double max_gap = 0, sum_pair = 0, pair_count = 0;
        for (std::size_t i = 0; i < game_vecs.size(); ++i)
            for (std::size_t j = i + 1; j < game_vecs.size(); ++j) {
                const double d = euclid(game_vecs[i], game_vecs[j]);
                max_gap = std::max(max_gap, d);
                sum_pair += d;
                pair_count += 1;
            }
        if (max_gap <= gap_eps) {
            out.rejected.push_back({sp.speakerId, "no-game-gap", count});
            continue;
        }
        AdoptCandidate c;
        c.speakerId = sp.speakerId;
        c.source = sp.source;
        c.affect = weighted_mean4(game_vecs, game_weights);
        c.count = count;
        c.polarityBias = js_round_fixed(std::fabs(pos - neg) / total, 4);
        c.affectDispersion = pair_count > 0 ? js_round_fixed(sum_pair / pair_count, 4) : 0;
        out.candidates.push_back(std::move(c));
    }
    return out;
}

AdoptResult adopt_personas(const std::vector<SpeakerOpinions>& speakers, const AdoptOptions& opts) {
    auto evaluated = evaluate_speakers(speakers, opts);

    // 母集団平均 = 既存 adopted + 今回候補の affect 平均 → typicality (cosine) を全 adopted で再計算。
    const auto existing = list_pool_personas({std::string("adopted"), std::nullopt});
    std::vector<std::vector<double>> all;
    for (const auto& p : existing) all.push_back(p.affectVector);
    for (const auto& c : evaluated.candidates) all.push_back(c.affect);
    const auto pop_mean = mean_of(all);

    AdoptResult out;
    for (const auto& c : evaluated.candidates) {
        PoolPersona persona;
        persona.id = crypto::uuid_v4();
        persona.name = crawler::sources::masked_persona_label(c.speakerId);
        persona.role = "opinion";
        persona.speechStyle = "";  // 口調は付けない (C1)
        persona.affectVector = c.affect;
        persona.origin = "adopted";
        persona.learningSource = c.source;
        persona.sourceSpeakerId = c.speakerId;
        persona.typicality = js_round_fixed(cosine(c.affect, pop_mean), 4);
        persona.polarityBias = c.polarityBias;
        persona.affectDispersion = c.affectDispersion;
        out.adopted.push_back(upsert_pool_persona_by_speaker(persona));
    }
    // 既存 adopted の typicality も新しい母集団平均で更新 (一貫性)。
    for (const auto& p : existing) set_pool_persona_typicality(p.id, js_round_fixed(cosine(p.affectVector, pop_mean), 4));
    out.rejected = std::move(evaluated.rejected);
    return out;
}

} // namespace di::flow
