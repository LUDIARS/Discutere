#pragma once
// 埋め込み画面 (native/web/**) の {{名前}} 印を置き換える。値のエスケープは呼び出し側の責任。
#include <map>
#include <string>
#include <string_view>

namespace di::api {

/** {{名前}} を 1 回の走査で置き換える (置き換えた値の中は再走査しない)。未知の印はそのまま残す。 */
std::string fill_template(std::string_view tpl, const std::map<std::string, std::string>& values);

} // namespace di::api
