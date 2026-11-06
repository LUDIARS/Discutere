#pragma once
// ルール指紋と output 同値判定 (fingerprint.ts)。純粋関数のみ。
// 指紋 = when + output の「キー順に依存しない」正規化 JSON (description は含めない)。
#include <string>

#include "base/json.hpp"

namespace di::lib::blackbox {

/**
 * 値を キー昇順・配列順保持 で決定的に直列化する (JSON.stringify(sortValue(v)) と同じ文字列)。
 * JS のオブジェクトは配列添字形式のキー ("0", "12" 等) を数値順で先に並べるので、それも再現する。
 */
std::string canonical_json(const Json& value);
/** ルールの論理内容の指紋。 */
std::string rule_fingerprint(const Json& when, const Json& output);
/** output 同士の同値判定 (影評価の一致/不一致)。 */
bool same_output(const Json& a, const Json& b);

} // namespace di::lib::blackbox
