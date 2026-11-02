#pragma once
// 議論設定 UI / API (src/api/tuning-routes.ts の移植)。index では /api に mount する。
//
//   GET  /admin/tuning              編集 HTML (native/web/api/tuning.html)
//   GET  /admin/tuning/data         現在値 JSON (store 未注入なら 503 {"enabled":false})
//   PUT  /admin/tuning/facilitator  収束トリガー更新 (Partial<FacilitatorTuning>)
//   PUT  /admin/tuning/role         役割プロンプト override {role, text|null}
//   PUT  /admin/tuning/rule         debate instructions override {ruleId, text|null}
//   PUT  /admin/tuning/cost-relay   コスト relay 設定 {enabled?, anatomiaUrl?, concordiaUrl?, service?}
//
// 認可は TS と同じく置かない (loopback 運用前提。:3100 の他の操作 UI と同じ)。
// store と再シードのフックは index が set_runtime_settings / set_rule_reseeder で後から注入する (late-binding)。
// YouTube キーは Excubitor Vault 管理で、変更・再取得の API は持たない (存在の有無だけ返す)。
#include <functional>
#include <memory>

#include "infra/http.hpp"
#include "runtime_settings/store.hpp"

namespace di::api {

void set_runtime_settings(std::shared_ptr<runtime_settings::RuntimeSettingsStore> store);
/** debate ルール instructions の再シードを依頼するフック (rule の override 更新時に呼ぶ)。 */
void set_rule_reseeder(std::function<void()> fn);

http::Router tuning_routes();

} // namespace di::api
