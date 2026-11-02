#pragma once
// src/api/noise-routes.ts の移植: ノイズ管理 API/UI (per-utterance soft-exclude, reversible)。"/api" に mount する前提。
// 議論データから別ゲームの雑談・人間の煽り等のノイズ発話を可逆的に除外する (utterance_exclusions サイドカー表)。
// Core は都度 open/close。全ルート admin role 必須 (userId 無し 401 / role≠admin 403)。
//   GET  /admin/noise             画面 (native/web/api/noise.html)
//   GET  /admin/noise/groups      議論 session 一覧 + ソース種別
//   GET  /admin/noise/utterances  session か source の発話 (除外フラグ付き、最大 300 件)
//   POST /admin/noise/exclude     {utteranceId, reason?}
//   POST /admin/noise/restore     {utteranceId}
#include "infra/http.hpp"

namespace di::api {

http::Router noise_routes();

} // namespace di::api
