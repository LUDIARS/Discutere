#pragma once
// 論拠の形を成していない発言の深掘り (spec/plan/2026-11-23-dialectic-drilldown.md)。
//
// 形は dialectic.md §2.5 の定義 (論拠の 3 種類・トゥールミンの整理)。形を満たさない応答・定立には、欠けている要素を
// 名指しして同じ人に 1 回だけ言い直させる。深掘りは論拠が不足している場合だけ (データ・論拠が無い / 論拠がデータの言い換え)。
// 突く先が無い・種類や限定が無い・前提ルール違反 (範囲外・すり替え等) は論拠の不足ではないので深掘りしない。ここは判定と文面だけを持つ (LLM 呼び出し・記録は exchange_runner / driver)。
#include <optional>
#include <string>
#include <vector>

#include "flow/dialectic/exchange_prompt.hpp"
#include "flow/dialectic/position.hpp"
#include "flow/dialectic/rule_check.hpp"

namespace di::flow::dialectic {

/** 深掘りで形を整え直せる違反 (形の検査がコードで見つけたもの)。 */
bool is_form_foul(const RuleFoul& foul);

/**
 * 反論の応答に欠けている要素 (§2.5 の語彙)。空なら形は満たしている。
 * JSON 崩れ・データ無し・論拠無し・突く先無しを見る。gate は形の検査の結果 (無ければ応答だけから判断)。
 */
std::vector<std::string> missing_rebut_elements(const ParsedExchangeTurn& turn, const std::optional<RebutGateResult>& gate);

/** 進行役が流す深掘りの定型文。 */
std::string render_drilldown_request(const std::string& persona_name, const std::vector<std::string>& missing);

/** 言い直しの応答プロンプトに足す節 (欠けている要素と前の応答の要旨)。 */
std::string drilldown_prompt_block(const std::vector<std::string>& missing, const std::string& previous_text);

/**
 * 定立に欠けている要素。JSON 崩れの degrade (根拠が主張の複製)・論拠 (warrant) の無い根拠・種類の無い根拠を見る。
 * LLM 障害による degrade (llm_error=true) は形の問題ではないので空を返す。
 */
std::vector<std::string> missing_position_elements(const GeneratedPosition& p, bool llm_error);

/** 定立の作り直しに渡す feedback (向きの作り直しと同じ経路)。 */
std::string position_drilldown_feedback(const std::vector<std::string>& missing);

} // namespace di::flow::dialectic
