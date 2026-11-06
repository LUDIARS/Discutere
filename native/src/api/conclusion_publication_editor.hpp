#pragma once
// 議論結果の公開設定のローカル操作画面 (src/api/conclusion-publication-editor.ts の移植)。
// 画面は移植しない: 本文は native/web/api/conclusion-publication-editor.html (議論の文字列は textContent で入れる)。
#include <string_view>

namespace di::api {

inline constexpr std::string_view kPublicationEditorAsset = "api/conclusion-publication-editor.html";

/** PUBLICATION_EDITOR の HTML。 */
std::string_view publication_editor_html();

} // namespace di::api
