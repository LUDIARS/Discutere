#pragma once
// @ludiars/sentiment-core vector.ts の移植: 20 次元の感情ベクトル (語彙辞書の部分一致と算術のみ・決定的)。
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::lib::sentiment_core {

using Vector = std::vector<double>;

/** 感情キー (Plutchik 8、辞書の順)。 */
const std::vector<std::string>& emo_keys();
/** アスペクトキー (ゲーム評価軸 8、辞書の順)。 */
const std::vector<std::string>& asp_keys();
/** VECTOR_SPEC (= VECTOR_DIMS): ["emo.valence", "emo.arousal", "emo.<感情>"..., "asp.<軸>"..., "meta.positive_ratio", "meta.volume_log"]。 */
const std::vector<std::string>& vector_spec();
inline const std::vector<std::string>& vector_dims() { return vector_spec(); }

/** 次元数 (VECTOR_SPEC.length)。辞書が 8 感情 + 8 軸であることを読み込み時に確かめる。 */
inline constexpr std::size_t DIM = 20;
inline constexpr int VECTOR_SPEC_VERSION = 1;

struct SentimentSignal {
    std::optional<bool> votedUp;
    std::optional<double> upvotes;
};

struct TextFeatures {
    double valence = 0;
    /** emo_keys() と同じ順 (0 / 1)。 */
    std::vector<double> emotions;
    /** asp_keys() と同じ順 (語彙の一致数)。 */
    std::vector<double> aspectMentions;
    double arousal = 0;
};

TextFeatures extract_text_features(std::string_view text, const SentimentSignal& signal = {});
Vector build_vector(const std::vector<TextFeatures>& features);
Vector text_to_vector(std::string_view text);

/** 次元が違えば std::invalid_argument ("vector dim mismatch: a vs b")。 */
double dot(const Vector& left, const Vector& right);
Vector subtract(const Vector& left, const Vector& right);
double norm(const Vector& v);
double scalar_projection(const Vector& v, const Vector& onto);
double cosine(const Vector& left, const Vector& right);
/** 数が違えば std::invalid_argument。空なら DIM 個の 0。 */
Vector weighted_mean(const std::vector<Vector>& vectors, const std::vector<double>& weights);

/** JSON 配列 (数値) にする。 */
Json vector_to_json(const Vector& v);

} // namespace di::lib::sentiment_core
