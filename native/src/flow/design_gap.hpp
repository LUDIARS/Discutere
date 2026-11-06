#pragma once
// src/flow/design-gap.ts の移植: design_gap 計算 (改善フロー / improvement.md (1))。
//
//   現状 (起点) ベクトル = Di 基礎感情データ (`*.sentiment.json` の game_vector)。起点は negative を想定する。
//   目標 ベクトル       = メカニクスの intended_* から固定 20 次元へ写像。
//   design_gap          = 目標 − 現状 = 目指す改善ベクトル (positive 方向)。
// ベクトル空間は sentiment_vector の固定 20 次元。新しい感情空間は定義しない。
// data/games のファイルは上限つきで読む (gray-matter → lib/front_matter)。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "flow/investigate.hpp"

namespace di::flow {

/** 現状 (起点) が欠落しているときの negative ベースライン。 */
std::vector<double> negative_baseline_vector();

struct LoadCurrentVectorOpts {
    std::function<void(const std::string&)> warn;
};

/**
 * テーマに対応する `*.sentiment.json` の game_vector を現状ベクトルとして読む。
 * テーマ語は game-aliases で別名展開してから照合する。見つからなければ warn を出して nullopt。
 */
std::optional<std::vector<double>> load_current_vector(const std::string& theme, const std::string& games_dir = "./data/games",
                                                       const LoadCurrentVectorOpts& opts = {});

/** メカニクス群から目標感情ベクトルを構築する。 */
std::vector<double> build_target_vector(const std::vector<MechanicSummary>& mechanics);

/** design_gap = 目標 − 現状。 */
std::vector<double> compute_design_gap(const std::vector<double>& current, const std::vector<double>& target);

} // namespace di::flow
