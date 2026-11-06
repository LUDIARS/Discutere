#pragma once
// JS の型変換 (Number(v) / String(v) / Boolean(v)) を JSON 値に対して行う補助。
// runtime-settings と tuning-routes が「受けた JSON を TS と同じ規則で数値・文字列にする」ために使う。
#include <string>

#include "base/json.hpp"

namespace di::runtime_settings::js {

/** Number(v)。解釈できなければ NaN。配列は join(",") した文字列として読む (JS と同じ)。 */
double to_number(const Json& value);
/** String(v)。object は "[object Object]"、配列は要素を "," で結合 (null は空)。 */
std::string to_string(const Json& value);
/** Boolean(v) (JS の truthy)。 */
bool truthy(const Json& value);
/** Math.round(n) (JS と同じく .5 は +∞ 方向)。 */
double round(double n);

} // namespace di::runtime_settings::js
