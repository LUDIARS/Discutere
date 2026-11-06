#pragma once
// 学習ビューの 3 層 (基礎知識 / ゲーム学習 / 話題と意見) のスナップショット (src/visualize/learning-layers.ts の移植)。
// 戻り値は TS の LearningLayerSnapshot と同じ形の JSON。
#include <optional>
#include <string>
#include <string_view>

#include "base/json.hpp"
#include "base/sqlite.hpp"

namespace di::visualize {

/** "knowledge" | "games" | "opinions" */
using LearningLayer = std::string;

struct LearningLayerOptions {
    std::optional<double> limit;
    std::optional<double> detailLimit;
    /** affect 語彙ファイル。空なら TS と同じ path.resolve("data/affects/vocabulary.json")。 */
    std::string vocabularyPath;
};

/** 語彙ファイルの上限 (読む前に大きさで弾く)。 */
inline constexpr std::size_t kAffectVocabularyMaxBytes = 4u << 20;

Json build_learning_layer_snapshot(sql::Database& db, const std::string& workspace_id, const LearningLayer& layer,
                                   const LearningLayerOptions& options = {});

/** 不明な値は "knowledge"。 */
LearningLayer normalize_learning_layer(const std::optional<std::string>& value);

} // namespace di::visualize
