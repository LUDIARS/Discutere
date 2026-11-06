#pragma once
// src/flow/improvement.ts の移植: 改善フロー (improvement.md / OVERVIEW §11-c)。
//
// 議論フローと骨格は同一 (run_flow を再利用)。差分は step 6 の世論決定を
// 中立投票でなく機械計算 (design_gap への射影) にする点のみ。
//   - step 1〜5・7〜9 は議論フローと同一 (FlowDirector の骨格そのまま)。
//   - step 6: 各意見の「採用した場合の体験変化」を LLM で予測 (effect_predict) し、
//     予測変化ベクトルを design_gap 方向へ射影してスコア。最高スコア = 世論。中立投票はしない。
//
// design_gap = 目標 (intended_affect) − 現状 (基礎感情データ, negative 想定)。
// LLM 効果予測を正とし、LLM 失敗時のみ意見単位で旧 lexicon 方式へフォールバックする
// (method 列に記録 + warn、議論は完走する)。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "flow/director.hpp"
#include "flow/improvement_score.hpp"
#include "flow/tags.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

struct ImprovementFlowDeps : FlowDirectorDeps {
    /**
     * 意見テキスト → 20 次元感情ベクトル (旧 lexicon 方式)。**LLM 効果予測が失敗した意見の
     * fallback にだけ**使う。既定は text_to_vector (辞書ベース)。テストで差し替え可能。
     */
    std::function<std::vector<double>(const std::string&)> opinionToVector;
    /**
     * 現状 (起点) ベクトルを明示指定する (テスト用)。
     * 未指定なら data/games の sentiment.json から読み (game-aliases で別名展開)、無ければ warn の上 negative ベースライン。
     */
    std::optional<std::vector<double>> currentVector;
};

/** improvement_score テーブルへスコアを記録する (method / detail_json 込み)。 */
void persist_improvement_scores(const std::string& session_id, int round, const std::vector<ScoredOpinion>& scored,
                                const std::optional<std::string>& winner);

/**
 * 改善フローを実行する。
 * 世論決定 = LLM 効果予測 → design_gap への射影による機械スコア (中立投票は行わない)。
 */
FlowDirectorResult run_improvement_flow(persona_engine::LLMClient& llm, const std::string& theme, const std::vector<FlowTag>& tags,
                                        const ImprovementFlowDeps& deps);

} // namespace di::flow
