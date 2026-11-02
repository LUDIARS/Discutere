#pragma once
// Admin API (PR-B, src/api/admin-routes.ts の移植) — 人間 → AI への介入経路。index では /api に mount する。
//
//   POST /admin/rules/enabled        {enabled}   engine.set_rules_enabled (議論自走を runtime で止める/再開)
//   POST /admin/session/reset        {sessionId} engine.reset_session (safety cap を解放)
//   POST /admin/gaps/:id/dismiss     未処理 gap を却下 (status='dismissed')
//   POST /admin/gaps/dismiss-open    未処理 gap を一括却下
//   POST /admin/sessions/:id/close   進行中の議論 session を閉じる (scene 単位 + gap closed)
//   POST /admin/sessions/:id/converge facilitator.converge_now で LLM まとめ → 収束
//   GET  /admin/status               kill switch 状態と直近 rule_log
//   POST /admin/seed-sweep           未検知投稿の再スイープ (gateway の sweeper)
//   GET  /admin/learning/topics      話題と意見のスナップショット
//   GET  /admin/metrics/personas | /admin/metrics/rules | /admin/rules | /admin/rules/:id/logs
//
// 認証: 全ルート admin role 必須 (middleware::require_admin。userId 無し 401 / role≠admin 403)。
// persona-engine / facilitator / sweeper は index が起動時に set_* で注入する (late-binding。未注入は 503)。
#include <functional>
#include <memory>
#include <optional>

#include "infra/http.hpp"
#include "persona_engine/facilitator/facilitator.hpp"
#include "persona_engine/index.hpp"

namespace di::api {

struct SweepResult {
    double scanned = 0;
    double seeded = 0;
};
/** gateway の sweepUnseeded。limit 指定なし (nullopt) は gateway 側の既定。失敗は例外。 */
using GatewaySweeper = std::function<SweepResult(std::optional<double> limit)>;

/** index などから注入 (起動時 1 回)。 */
void set_persona_engine(std::shared_ptr<persona_engine::PersonaEngineHandle> engine);
std::shared_ptr<persona_engine::PersonaEngineHandle> get_persona_engine();
/** facilitator インスタンスを注入 (手動収束 = まとめて閉じる で使う)。 */
void set_facilitator(std::shared_ptr<persona_engine::facilitator::Facilitator> facilitator);
/** gateway の sweepUnseeded を注入 (gateway 起動後)。未注入なら seed-sweep は 503。 */
void set_gateway_sweeper(GatewaySweeper sweeper);

http::Router admin_routes();

} // namespace di::api
