#pragma once
// src/api/kg-migration-routes.ts の移植: KG 共有同期の配信エンドポイント (master 側) — spec/data/core/KG-SYNC.md。
//
// GET /kg/migrations?since=<epochMs>[&token=<secret>] ("/api" に mount する前提)
//   共有テーブルの shared=1 かつ updated_at>since の行を NDJSON で返す。1 行目 manifest の watermark を follower が保存する。
// 認可: config.kgSync.serveToken が設定されている場合のみ ?token= か x-kg-token を要求する (違えば 403 "forbidden")。
//   未設定なら誰でも取得可 (ローカル/Tailnet 前提、TS と同じ)。比較は定数時間 (移植で TS の === から強めた)。
#include <functional>
#include <optional>
#include <string>

#include "base/sqlite.hpp"
#include "infra/http.hpp"
#include "kg_sync/export.hpp"

namespace di::api {

struct KgMigrationRouteDeps {
    /** 配信トークン (既定 config.kgSync.serveToken)。 */
    std::function<std::optional<std::string>()> serveToken;
    /** 共有 KG を差分エクスポートする (既定は active KG を開いて export_shared_kg)。 */
    std::function<kg_sync::KgExportResult(double since)> exportShared;
};

http::Router kg_migration_routes(KgMigrationRouteDeps deps = {});

} // namespace di::api
