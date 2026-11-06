#include "lib/sentiment_core/vector.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "base/strings.hpp"
#include "lib/sentiment_core/js_number.hpp"
#include "lib/sentiment_core/lexicon.hpp"

namespace di::lib::sentiment_core {

namespace {

struct Keys {
    std::vector<std::string> emo;
    std::vector<std::string> asp;
    std::vector<std::string> spec;
    // 照合用に小文字化した語彙 (TS は毎回 word.toLowerCase() する。結果は同じ)
    std::vector<std::pair<std::string, double>> polarity_lower;
    std::vector<std::vector<std::string>> emotion_words;
    std::vector<std::vector<std::string>> aspect_words;
    std::vector<std::string> arousal_words;
};

const Keys& keys() {
    static const Keys k = [] {
        const auto& lex = sentiment_lexicon();
        Keys out;
        for (const auto& [name, words] : lex.emotions) {
            out.emo.push_back(name);
            std::vector<std::string> lower;
            for (const auto& w : words) lower.push_back(str::to_lower(w));
            out.emotion_words.push_back(std::move(lower));
        }
        for (const auto& [name, words] : lex.aspects) {
            out.asp.push_back(name);
            std::vector<std::string> lower;
            for (const auto& w : words) lower.push_back(str::to_lower(w));
            out.aspect_words.push_back(std::move(lower));
        }
        for (const auto& [w, s] : lex.polarity) out.polarity_lower.emplace_back(str::to_lower(w), s);
        for (const auto& w : lex.arousal) out.arousal_words.push_back(str::to_lower(w));
        out.spec = {"emo.valence", "emo.arousal"};
        for (const auto& e : out.emo) out.spec.push_back("emo." + e);
        for (const auto& a : out.asp) out.spec.push_back("asp." + a);
        out.spec.push_back("meta.positive_ratio");
        out.spec.push_back("meta.volume_log");
        if (out.spec.size() != DIM) throw std::runtime_error("sentiment lexicon must define 8 emotions and 8 aspects");
        return out;
    }();
    return k;
}

double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }
double round4(double v) { return js_round_fixed(v, 4); }

// JS の includes は空文字に一致する ("".includes("") も true)。
bool includes(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

double count_hits(const std::string& text, const std::vector<std::string>& words) {
    double sum = 0;
    for (const auto& w : words) sum += includes(text, w) ? 1 : 0;
    return sum;
}

void assert_same_dimension(const Vector& left, const Vector& right) {
    if (left.size() != right.size())
        throw std::invalid_argument("vector dim mismatch: " + std::to_string(left.size()) + " vs " +
                                    std::to_string(right.size()));
}

} // namespace

const std::vector<std::string>& emo_keys() { return keys().emo; }
const std::vector<std::string>& asp_keys() { return keys().asp; }
const std::vector<std::string>& vector_spec() { return keys().spec; }

TextFeatures extract_text_features(std::string_view text, const SentimentSignal& signal) {
    const Keys& k = keys();
    const std::string normalized = str::to_lower(text);
    double polarity = 0;
    double polarity_hits = 0;
    for (const auto& [word, score] : k.polarity_lower) {
        if (includes(normalized, word)) {
            polarity += score;
            polarity_hits += 1;
        }
    }
    if (signal.votedUp == true) {
        polarity += 1.5;
        polarity_hits += 1;
    } else if (signal.votedUp == false) {
        polarity -= 1.5;
        polarity_hits += 1;
    }
    TextFeatures f;
    f.valence = polarity_hits != 0 ? std::max(-1.0, std::min(1.0, polarity / std::max(2.0, polarity_hits))) : 0;
    for (const auto& words : k.emotion_words) f.emotions.push_back(count_hits(normalized, words) > 0 ? 1 : 0);
    for (const auto& words : k.aspect_words) f.aspectMentions.push_back(count_hits(normalized, words));
    // (text || "").split("!").length - 1 = "!" の個数
    const double bangs = static_cast<double>(std::count(text.begin(), text.end(), '!'));
    f.arousal = clamp01((count_hits(normalized, k.arousal_words) + bangs) / 4);
    return f;
}

Vector build_vector(const std::vector<TextFeatures>& features) {
    const Keys& k = keys();
    const double divisor = features.empty() ? 1.0 : static_cast<double>(features.size());
    double valence_sum = 0;
    double arousal_sum = 0;
    for (const auto& f : features) valence_sum += f.valence;
    for (const auto& f : features) arousal_sum += f.arousal;
    const double mean_valence = valence_sum / divisor;
    const double arousal = arousal_sum / divisor;
    Vector out;
    out.push_back(clamp01((mean_valence + 1) / 2));
    out.push_back(arousal);
    for (std::size_t e = 0; e < k.emo.size(); ++e) {
        double sum = 0;
        for (const auto& f : features) sum += e < f.emotions.size() ? f.emotions[e] : 0;
        out.push_back(sum / divisor);
    }
    for (std::size_t a = 0; a < k.asp.size(); ++a) {
        double positive = 0, negative = 0, mentioned = 0;
        for (const auto& f : features) {
            const double m = a < f.aspectMentions.size() ? f.aspectMentions[a] : 0;
            if (m == 0) continue;
            mentioned += 1;
            if (f.valence > 0.05) positive += 1;
            else if (f.valence < -0.05) negative += 1;
        }
        out.push_back(mentioned != 0 ? clamp01(0.5 + (0.5 * (positive - negative)) / mentioned) : 0.5);
    }
    double positive_count = 0;
    for (const auto& f : features)
        if (f.valence > 0.05) positive_count += 1;
    out.push_back(positive_count / divisor);
    out.push_back(clamp01(std::log10(static_cast<double>(features.size()) + 1) / 4));
    for (auto& v : out) v = round4(v);
    return out;
}

Vector text_to_vector(std::string_view text) { return build_vector({extract_text_features(text)}); }

double dot(const Vector& left, const Vector& right) {
    assert_same_dimension(left, right);
    double sum = 0;
    for (std::size_t i = 0; i < left.size(); ++i) sum += left[i] * right[i];
    return sum;
}

Vector subtract(const Vector& left, const Vector& right) {
    assert_same_dimension(left, right);
    Vector out(left.size());
    for (std::size_t i = 0; i < left.size(); ++i) out[i] = left[i] - right[i];
    return out;
}

double norm(const Vector& v) { return std::sqrt(dot(v, v)); }

double scalar_projection(const Vector& v, const Vector& onto) {
    const double magnitude = norm(onto);
    return magnitude == 0 ? 0 : dot(v, onto) / magnitude;
}

double cosine(const Vector& left, const Vector& right) {
    const double ln = norm(left);
    const double rn = norm(right);
    return ln == 0 || rn == 0 ? 0 : dot(left, right) / (ln * rn);
}

Vector weighted_mean(const std::vector<Vector>& vectors, const std::vector<double>& weights) {
    if (vectors.size() != weights.size())
        throw std::invalid_argument("vector/weight count mismatch: " + std::to_string(vectors.size()) + " vs " +
                                    std::to_string(weights.size()));
    if (vectors.empty()) return Vector(DIM, 0.0);
    for (const auto& v : vectors)
        if (v.size() != vectors[0].size())
            throw std::invalid_argument("weighted mean requires vectors with the same dimension");
    double weight_sum = 0;
    for (double w : weights) weight_sum += w;
    if (weight_sum == 0 || std::isnan(weight_sum)) weight_sum = 1;  // `|| 1`
    Vector out(vectors[0].size());
    for (std::size_t d = 0; d < out.size(); ++d) {
        double sum = 0;
        for (std::size_t i = 0; i < vectors.size(); ++i) sum += vectors[i][d] * weights[i];
        out[d] = round4(sum / weight_sum);
    }
    return out;
}

Json vector_to_json(const Vector& v) {
    Json out = Json::array();
    for (double x : v) out.push_back(x);
    return out;
}

} // namespace di::lib::sentiment_core
