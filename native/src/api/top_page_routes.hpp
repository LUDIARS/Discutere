#pragma once
// トップページ (GET /) — 各 Web UI への入口 (src/api/top-page-routes.ts の移植)。
// loopback 運用前提で guard 無し (TS と同じ)。HTML は native/web/api/top-page.html。
#include "infra/http.hpp"

namespace di::api {

http::Router top_page_routes();

} // namespace di::api
