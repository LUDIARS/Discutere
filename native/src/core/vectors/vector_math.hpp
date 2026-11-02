#pragma once
// 埋め込みベクトルの I/O 検証と類似度計算 (src/core/vectors/vector-math.ts)。
#include <optional>
#include <vector>

#include "base/json.hpp"

namespace di::core::vectors {

/** 外部入力・永続化データを計算へ渡せる有限数ベクトルか検証する (空でない・全要素が有限数)。 */
bool is_embedding_vector(const Json& value);

/** Json の配列を double のベクトルにする (is_embedding_vector が真のときだけ値を返す)。 */
std::optional<std::vector<double>> to_embedding_vector(const Json& value);

/** 次元が異なるベクトルは別空間として類似度 0 に倒す。 */
double cosine_similarity(const std::vector<double>& a, const std::vector<double>& b);

} // namespace di::core::vectors
