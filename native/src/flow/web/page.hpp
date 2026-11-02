#pragma once
// /flow の画面 (src/flow/web/page.ts の FLOW_HTML)。画面は移植しない: 本文は native/web/flow/page.html
// (native/tests/golden/w1-F-golden.gen.ts が Node の文字列をそのまま書き出したもの。サーバ側の値は埋め込まない)。
#include <string_view>

namespace di::flow::web {

inline constexpr std::string_view kFlowPageAsset = "flow/page.html";

/** FLOW_HTML。 */
std::string_view flow_html();

} // namespace di::flow::web
