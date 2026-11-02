#pragma once
// Voluptas → Discutere の感想受け取り口 `POST /persona-bridge/impressions` ("/api" に mount する前提)
// (src/api/impression-bridge-routes.ts の移植、spec/feature/flow/voluptas-impression-push.md)。
//
// Voluptas が自動で送る匿名の感想を、Cernere service token (aud=Discutere の storage_slug、
// scope=persona-bridge:write) か移行期間の固定トークンで受ける (persona_bridge/bridge_auth)。
// 本文 `{ impressions: [...] }` → 匿名性・形の検査 → KG へ取込 (dedup) → 件数を返す。
// 応答はすべて `cache-control: private, no-store`。内部例外の本文は応答にもログにも出さない。
#include <cstdint>
#include <functional>
#include <string>

#include "cernere_service_token/cernere_public_keys.hpp"
#include "infra/http.hpp"
#include "persona_bridge/impression_intake.hpp"

namespace di::api {

using WithIngestFn = std::function<void(const std::function<void(const persona_bridge::IngestDeps&)>& run)>;

struct ImpressionBridgeRoutesOptions {
    /** 固定トークン (既定 env DISCUTERE_PERSONA_BRIDGE_TOKEN)。 */
    std::function<std::string()> token;
    /** service token の aud (既定 env CERNERE_PROJECT_STORAGE_SLUG)。 */
    std::function<std::string()> serviceAudience;
    /** 既定 Cernere 公開鍵ローダ (CERNERE_BASE_URL)。 */
    cernere_service_token::PublicKeyLoader loadServiceTokenKeys;
    std::function<std::int64_t()> now;
    /** 取込依存を用意して run を呼ぶ (テストで差し替える)。既定は active KG を開いて importer + 埋め込み。 */
    WithIngestFn withIngest;
};

/** 本番の取込 (active KG を開いて importer + 埋め込み)。 */
void default_impression_ingest(const std::function<void(const persona_bridge::IngestDeps&)>& run);

http::Router create_impression_bridge_routes(ImpressionBridgeRoutesOptions options = {});

} // namespace di::api
