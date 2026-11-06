#pragma once
// src/api/queue-routes.ts の移植: 議論キュー可視化 API。
// GET /admin/queue ("/api" に mount する前提) — 進行中 session / 未処理 gap / 検証待ち仮説のスナップショット。
// admin role 必須 (userId 無し 401 / role≠admin 403、middleware::require_admin)。
// Core は都度 open/close。persona-engine db は読み取り専用で開き、開けなければ fireCount=0。
#include "infra/http.hpp"

namespace di::api {

http::Router queue_routes();

} // namespace di::api
