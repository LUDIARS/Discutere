#pragma once
// Condition AST の評価 + 妥当性チェック (condition.ts)。純粋関数のみ。
#include <string>

#include "lib/blackbox/types.hpp"

namespace di::lib::blackbox {

/** Condition を features に対して評価する。未知 feature や型不一致・壊れた条件は false。 */
bool evaluate(const Condition& cond, const FeatureMap& features);

/**
 * 信頼できない入力 (LLM 生成 / API body) から来た Condition を検証して正規化した形を返す。
 * 不正なら理由つきの std::invalid_argument。深さは DoS 対策で 8 段まで。
 */
Condition validate_condition(const Json& value, int depth = 0);

/** Condition を人間可読な短文に整形する (UI / 通知のルール説明用)。 */
std::string describe_condition(const Condition& cond);

/** JS の String(value) (number は Number#toString、配列は "," 結合、object は "[object Object]")。 */
std::string js_string(const Json& value);

} // namespace di::lib::blackbox
