#pragma once
// 合意スコア可視化 (src/api/consensus-routes.ts の移植、FEATURE ③+④)。index では /api に mount する。
//
// - GET /admin/consensus           → ライブ管制ページ (native/web/api/consensus.html)
// - GET /admin/consensus/gaps      → 直近の議論 gap 一覧 (ドロップダウンのソース)
// - GET /admin/consensus/:gapId    → その議論の意見 + consensus_scores (人間は「人間」タグ)
//
// 人間 / AI の意見を同一レイヤで並べる (区別せず score 順)。admin role 必須 (401/403)。
// Discatier Core は都度 open/close (queue-routes と同じ短命接続)。
// consensus_persona_routes / gap_creation_routes (w3-M) を同じ Router の先頭に載せる (TS の route("/", ...))。
#include <functional>
#include <memory>

#include "core/index.hpp"
#include "infra/http.hpp"

namespace di::api {

/** active KG を開く関数 (既定は config の active KG)。 */
using CoreOpener = std::function<std::unique_ptr<core::Core>()>;

/** active KG (core::resolve_active_kg_path(config)) を開く既定の opener。 */
std::unique_ptr<core::Core> open_active_core();

http::Router consensus_routes(CoreOpener open_core = nullptr);

} // namespace di::api
