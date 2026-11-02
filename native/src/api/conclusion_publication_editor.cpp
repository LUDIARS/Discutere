#include "api/conclusion_publication_editor.hpp"

#include "api/web_page.hpp"

namespace di::api {

std::string_view publication_editor_html() { return web_page(kPublicationEditorAsset); }

} // namespace di::api
