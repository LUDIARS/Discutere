#pragma once
// native/web/** をビルド時に埋め込んだ資産 (cmake/EmbedWeb.cmake が生成)。
// name は native/web からの相対パス (例 "flow/page.html")。無ければ nullptr。
#include <string_view>

namespace di::web {

struct Asset {
    std::string_view content_type;
    std::string_view body;
};

const Asset* find(std::string_view name);

} // namespace di::web
