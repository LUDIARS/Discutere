#pragma once
// エラー可視化 API (src/api/errors-routes.ts の移植)。admin role 必須 (401/403)。
//   GET  /admin/errors?limit=100  → { errors: BufferedError[] } (新しい順、limit は 1..200)
//   POST /admin/errors/clear      → clear + { ok: true }
#include "infra/http.hpp"

namespace di::api {

http::Router errors_routes();

} // namespace di::api
