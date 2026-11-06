#pragma once
// 常駐ワーカー callback endpoint (内部用。src/api/worker.ts)。
//
// ワーカー (Lictor セッション) が worker-home の node スクリプト (scripts/{register,send}.mjs) から叩く:
//   - POST /internal/worker/register  { workerId, lictorPort, lictorPid? } : 起動時の自己登録
//   - POST /internal/worker/utterance { reqId, workerId, text, usage? }   : 1 ターンの発話
//
// 認証ミドルウェア (userContext) より前に mount し、Discord/Cernere 認証は要求しない。代わりに呼び元を loopback に
// 限定するガード (非 loopback は 403 {"error":"loopback only"}) を経路全体に噛ませる (H-1 多層防御)。
// ルータは "/internal" に mount する前提 (パターンは "/worker/...")。WorkerPool は set_worker_pool で後から注入する。
#include <memory>
#include <optional>

#include "base/json.hpp"
#include "infra/http.hpp"
#include "persona_engine/llm/client.hpp"
#include "persona_engine/worker_pool/pool.hpp"

namespace di::api {

/** main から WorkerPool を注入 (backend=worker-pool 時のみ非 null)。 */
void set_worker_pool(std::shared_ptr<persona_engine::worker_pool::WorkerPool> pool);

/**
 * callback body の usage (send.mjs が transcript から拾った token) を既知の数値フィールドだけに絞って TokenUsage 化する (#135)。
 * 未知/非数値は捨てる。トークンが 1 つも無ければ nullopt。テスト用に公開。
 */
std::optional<persona_engine::TokenUsage> pick_usage(const Json& raw);

/** callback 本文の JSON 上限。 */
JsonLimits worker_callback_json_limits();

http::Router worker_routes();

} // namespace di::api
