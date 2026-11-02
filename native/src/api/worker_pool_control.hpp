#pragma once
// 常駐ワーカープールの制御 UI / API (src/api/worker-pool-control.ts)。"/api" に mount する前提。
// boot 時自動 spawn ではなく、ここから手動で「何体・いつ起動/停止」を制御する
// (8 体一斉起動でサブスク枠を食い尽くす事故 (2026-06-06) の再発防止)。
//
//   GET  /api/worker-pool            制御 HTML (native/web/api/worker-pool-control.html)
//   GET  /api/worker-pool/status     JSON 状態
//   POST /api/worker-pool/start      全ワーカー起動
//   POST /api/worker-pool/stop       全ワーカー停止 (kill)
//   POST /api/worker-pool/worker/:id/start
//   POST /api/worker-pool/worker/:id/stop
#include <memory>

#include "infra/http.hpp"
#include "persona_engine/worker_pool/pool.hpp"

namespace di::api {

/** main から WorkerPool を注入 (backend=worker-pool 時のみ非 null)。 */
void set_worker_pool_control(std::shared_ptr<persona_engine::worker_pool::WorkerPool> pool);

http::Router worker_pool_control_routes();

} // namespace di::api
