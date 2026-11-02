#pragma once
// @implements DI-EXTERNAL-DISCUSSION
// POST /consider (src/api/external-discussion-routes.ts)。"/internal/external-discussion" に mount する前提。
// - 本文上限 160,000 バイト (Hono の bodyLimit({maxSize:160_000}) と同じ。超えたら 413)。
// - service か共有秘密が無ければ 503 {"error":"external discussion unavailable"}。
// - Authorization: Bearer <secret> を定数時間で比較 (違えば 401)。
// - 会話が不正なら 400、同時 4 本を超えたら 429、生成に失敗したら 503。
#include <cstddef>
#include <memory>
#include <string>

#include "external_discussion/service.hpp"
#include "infra/http.hpp"

namespace di::api {

inline constexpr std::size_t kExternalDiscussionMaxBody = 160'000;
inline constexpr int kExternalDiscussionMaxRunning = 4;

http::Router external_discussion_routes(std::shared_ptr<external_discussion::ExternalDiscussionService> service, std::string secret);

} // namespace di::api
