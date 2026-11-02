#pragma once
// src/flow/effect-predict.ts の移植: 効果予測 (改善フロー スコアリング、improvement.md (2))。
// 小モデル LLM に「この意見を採用した場合、体験の 20 次元がどう動くか」を構造化 JSON で予測させる。
// LLM 失敗 / パース失敗は nullopt (呼び出し側が lexicon 方式へフォールバック)。DB 非依存。
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"
#include "flow/investigate.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

/** LLM の応答 JSON を読むときの上限 (応答 1 件ぶん)。 */
JsonLimits llm_json_limits();

/**
 * 文字列から最初の JSON オブジェクトを取り出す (コードフェンス/前置き混入に耐える)。
 * まず最初の `{` 〜 最後の `}` の全体、失敗したらブレースマッチで先頭オブジェクトを救済。object 以外は nullopt。
 */
std::optional<Json> extract_json_object(std::string_view raw);

/** 予測された 1 次元の変化。dim は VECTOR_DIMS の次元名、delta は -1..1。 */
struct EffectChange {
    std::string dim;
    double delta = 0;
    std::optional<std::string> reason;
};
Json to_json(const EffectChange& c);
EffectChange effect_change_from_json(const Json& j);

struct EffectPrediction {
    std::vector<EffectChange> changes;
    /** "high" | "low" */
    std::string confidence = "high";
};
Json to_json(const EffectPrediction& p);

using WarnFn = std::function<void(const std::string&)>;

/** パース済み JSON を正規化する (未知の次元は捨てる、delta は -1..1)。changes が配列でなければ nullopt。 */
std::optional<EffectPrediction> normalize_prediction(const Json& raw, const WarnFn& warn = {});

/** 予測変化 (言及次元のみ) を 20 次元の移動ベクトルにする (重複は後勝ち)。 */
std::vector<double> changes_to_vector(const std::vector<EffectChange>& changes);

struct PredictEffectArgs {
    std::string opinion;
    std::string theme;
    std::vector<MechanicSummary> mechanics;
    /** 効果予測モデル。"" / 未指定なら LLM 既定。 */
    std::optional<std::string> model;
    WarnFn warn;
};

/** 意見の採用効果を LLM で予測する。失敗は nullopt。 */
std::optional<EffectPrediction> predict_effect(persona_engine::LLMClient& llm, const PredictEffectArgs& args);

} // namespace di::flow
