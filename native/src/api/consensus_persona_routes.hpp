#pragma once
// persona 別の合意スコア API (src/api/consensus-persona-routes.ts の移植)。index では /api に mount する。
//   GET /admin/consensus/:gapId/personas — admin role 必須 (401/403)。Cache-Control: private, no-store。
// @implements SPEC-THALEIA-PERSONA-CONSENSUS
// DB の失敗はローカルのパスや SQL を含みうるので応答に出さず 503 {"error":"persona consensus unavailable"}。
#include "infra/http.hpp"

namespace di::api {

http::Router consensus_persona_routes();

} // namespace di::api
