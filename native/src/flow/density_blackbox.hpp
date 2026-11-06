#pragma once
// 情報密度判定の成長型ブラックボックス化 (src/flow/density-blackbox.ts の移植、lib/blackbox)。
//
// evaluate_information_density (毎回 LLM 1 コール) を包み、判定を特徴量
// (voiceCount / sourceKinds / avgLen / tagCount) ベースのルールへ蒸留する。
// LLM が提案した閾値ルールは candidate として影評価で信頼を積み、trial 発火 → admin の OK×3 で auto 卒業
// = 以後この密度判定に LLM を呼ばない。ルール経路では gaps は返せないが、run_information_gate は
// gaps 空のとき theme をクエリにクロールするので degrade で回る。
#include <string>
#include <vector>

#include "base/sqlite.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/information_gate.hpp"
#include "flow/tags.hpp"
#include "lib/blackbox/blackbox.hpp"

namespace di::flow {

inline constexpr const char* DOMAIN_DENSITY = "flow.information_density";

/** 密度判定をルール化するためのフラット特徴量 (voiceCount, sourceKinds, avgLen, tagCount)。 */
lib::blackbox::FeatureMap density_features(const std::vector<ContextVoice>& voices, const std::vector<FlowTag>& tags);

/** LLM 評価の proposedRule (raw) を検証して blackbox 提案形式に直す。不正なら nullopt。 */
std::optional<lib::blackbox::RuleProposal> parse_proposed_rule(const std::optional<Json>& raw, const InformationDensity& density);

/** flow DB (discutere.db) 上の blackbox を束ねる。 */
lib::blackbox::BlackBox make_density_blackbox(const sql::DatabasePtr& db);

/**
 * evaluate_information_density 互換の評価器を作る。
 * 卒業/試用ルールが features に一致すれば LLM を呼ばず即決 (gaps は空 = theme クエリで degrade)。
 * ルールが無ければ従来どおり LLM 評価し、その判定を教師に candidate を育てる。
 */
EvaluateFn make_blackbox_density_evaluator(lib::blackbox::BlackBox bb, EvaluateFn evaluate_fn = evaluate_information_density);

} // namespace di::flow
