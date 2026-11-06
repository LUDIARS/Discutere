#pragma once
// JS の数値変換の補助 (sentiment-core と flow が使う)。
//   js_to_fixed(x, f)  … Number#toFixed(f) と同じ文字列 (正確な 10 進展開で四捨五入、同点は大きい方)
//   js_round_fixed(x, f) … Number(x.toFixed(f))
//   js_to_number(json)  … JS の Number(v) (JSON の値に対して)
#include <string>

#include "base/json.hpp"

namespace di::lib::sentiment_core {

std::string js_to_fixed(double value, int digits);
double js_round_fixed(double value, int digits);
/** Number(v): number はそのまま、string は Number(s) (読めなければ NaN)、bool は 1/0、null は 0、
 *  配列は空→0 / 1 要素→その要素の Number / それ以外 NaN、object は NaN。 */
double js_to_number(const Json& value);

} // namespace di::lib::sentiment_core
