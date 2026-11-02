#pragma once
// src/cernere-service-token/cernere-public-keys.ts の移植: Cernere の /.well-known/cernere-public-key を取得し、
// プロセスのメモリにキャッシュする (現行鍵 + ローテーション中の旧鍵)。10 分で取り直し、失敗直後は 30 秒待つ。
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "cernere_service_token/paseto_v4_public.hpp"
#include "infra/http_client.hpp"

namespace di::cernere_service_token {

/** 公開鍵を返す (取れなければ例外)。 */
using PublicKeyLoader = std::function<std::vector<Ed25519PublicKey>()>;
/** fetch の注入境界 (既定 http::fetch)。 */
using HttpFetch = std::function<http::ClientResponse(const http::ClientRequest&)>;

/** well-known の応答 ({keys: [{public_key: base64}]}) から鍵を取り出す。keys が無ければ std::runtime_error。 */
std::vector<Ed25519PublicKey> parse_well_known_keys(const Json& body);

inline constexpr std::int64_t kPublicKeyCacheTtlMs = 10 * 60 * 1000;
inline constexpr std::int64_t kPublicKeyFailureBackoffMs = 30 * 1000;
inline constexpr std::size_t kPublicKeyMaxResponseBytes = 256 * 1024;

struct CernerePublicKeyLoaderOptions {
    std::function<std::string()> baseUrl;
    HttpFetch fetchImpl;
    std::function<std::int64_t()> now;
};

/** createCernerePublicKeyLoader。返す関数はスレッド安全 (キャッシュは mutex で守る)。 */
PublicKeyLoader create_cernere_public_key_loader(CernerePublicKeyLoaderOptions options);

} // namespace di::cernere_service_token
