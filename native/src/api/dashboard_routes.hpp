#pragma once
// Admin dashboard (src/api/dashboard-routes.ts の移植)。
//   GET /admin/dashboard — 管制ダッシュボードの HTML (native/web/api/dashboard.html)。admin role 必須 (401/403)。
// index では /api に mount する。
#include "infra/http.hpp"

namespace di::api {

http::Router dashboard_routes();

} // namespace di::api
