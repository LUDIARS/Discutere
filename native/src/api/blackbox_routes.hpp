#pragma once
// /api/admin/blackbox — 成長型ブラックボックスのレビュー & ルール管理 (src/api/blackbox-routes.ts の移植、admin)。
//
// 情報密度判定 (flow.information_density) 等の blackbox 判断に OK/NG を返し、
// ルールの trial → auto 卒業 / 撤回を進める。実体は lib/blackbox。index では /api に mount する。
//
//   GET  /admin/blackbox/decisions              レビュー待ちキュー
//   POST /admin/blackbox/decisions/:id/verdict  { verdict: "ok"|"ng" }
//   GET  /admin/blackbox/rules?domain=          ルール一覧 + 卒業メトリクス
//   POST /admin/blackbox/rules/:id/state        { state } 手動昇格/撤回/復活
//
// 全ルートで admin role 必須 (userId 無し 401 / role≠admin 403、middleware::require_admin)。
#include <functional>

#include "infra/http.hpp"
#include "lib/blackbox/blackbox.hpp"

namespace di::api {

/** blackbox の取得 (既定は flow DB 上の make_density_blackbox。初回に作って以後同じものを使う)。 */
using BlackBoxFactory = std::function<lib::blackbox::BlackBox()>;

/** factory 省略時は flow DB (get_flow_db) の density blackbox。 */
http::Router blackbox_routes(BlackBoxFactory factory = nullptr);

/** テスト用: インスタンスをリセット (DB 差し替えに追従)。 */
void reset_blackbox_routes();

} // namespace di::api
