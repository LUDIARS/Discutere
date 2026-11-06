#include "flow/web/page.hpp"

#include "api/web_page.hpp"

namespace di::flow::web {

std::string_view flow_html() { return api::web_page(kFlowPageAsset); }

} // namespace di::flow::web
