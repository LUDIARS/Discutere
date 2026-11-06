#pragma once
// JS の Math 関数と同じ値を返す数学関数 (Node の出力と 1 ビットも違わないことが要るところで使う)。
// V8 の Math.log / Math.log10 は fdlibm の移植なので、同じ手順をここに置く。std::log10 は処理系の
// 実装に依り、MSVC では最後の 1 ビットが V8 と違うことがある (例: log10(11))。
namespace di::lib::sentiment_core {

/** Math.log と同じ値。 */
double js_log(double x);
/** Math.log10 と同じ値。 */
double js_log10(double x);

} // namespace di::lib::sentiment_core
