#pragma once
// 議題 (design gap) の手動作成 API (src/api/gap-creation-routes.ts の移植)。index では /api に mount する。
//   POST /admin/gaps {title, description, gameId?} — admin role 必須 (401/403)、Content-Type: application/json 以外は 415、
//   入力不正は 400、成功は 201 {gapId, sessionId} (headless 議論を立てる)。
// @implements SPEC-THALEIA-GAP-CREATION
// DB の失敗はローカルのパスや SQL を含みうるので応答に出さず 503 {"error":"gap creation unavailable"}。
#include <optional>
#include <string>

#include "base/json.hpp"
#include "infra/http.hpp"

namespace di::api {

struct GapCreationInput {
    std::string title;
    std::string description;
    std::optional<std::string> gameId;
};

/** 本文の検証 (title/description は空白だけでない文字列、gameId は与えるなら空白だけでない文字列)。trim して返す。 */
std::optional<GapCreationInput> parse_gap_creation_input(const std::optional<Json>& body);

http::Router gap_creation_routes();

} // namespace di::api
