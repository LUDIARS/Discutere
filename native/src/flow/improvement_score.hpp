#pragma once
// src/flow/improvement-score.ts の移植: 改善フローの機械スコアリング (improvement.md (2))。
// score(意見) = projection(movement, design_gap)。movement は LLM 効果予測の変化ベクトル (失敗時は lexicon)。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "flow/effect_predict.hpp"

namespace di::flow {

/** "effect-predict" | "lexicon-fallback" */
using ScoreMethod = std::string;

struct MovementCandidate {
    std::string id;
    /** 意見を採用した場合の移動 (予測変化) ベクトル。20 次元。 */
    std::vector<double> movement;
    ScoreMethod method;
    std::optional<std::vector<EffectChange>> detail;
};

struct ScoredOpinion {
    std::string id;
    double score = 0;
    ScoreMethod method;
    std::optional<std::vector<EffectChange>> detail;
};
Json to_json(const ScoredOpinion& s);

double score_movement(const std::vector<double>& movement, const std::vector<double>& design_gap);
/** 旧 lexicon 方式の 1 意見スコア (fallback 用)。 */
double score_opinion(const std::vector<double>& opinion_vec, const std::vector<double>& current_vec,
                     const std::vector<double>& design_gap);
std::vector<ScoredOpinion> score_opinions(const std::vector<MovementCandidate>& candidates,
                                          const std::vector<double>& design_gap);
/** 最高スコアの意見 id (同点は先着)。候補が無ければ nullopt。 */
std::optional<std::string> pick_winner(const std::vector<ScoredOpinion>& scored);

} // namespace di::flow
