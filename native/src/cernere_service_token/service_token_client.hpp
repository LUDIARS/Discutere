#pragma once
// src/cernere-service-token/service-token-client.ts の移植 (送り側): Cernere POST /api/auth/service-token で
// service token を取得する。
// - project client credentials は Cernere にだけ送る (baseUrl は HTTPS、開発用 loopback のみ HTTP)。
// - token は exp - 60 秒までプロセスのメモリにだけキャッシュし、ディスク・ログへ書かない。
// - 失敗は理由コードで返す。
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include "infra/http_client.hpp"

namespace di::cernere_service_token {

/** "base_url_missing" | "base_url_insecure" | "credentials_missing" | "unauthorized" | "scope_undeclared" |
 *  "target_not_found" | "issuer_error" | "malformed_response" | "network" */
using ServiceTokenFailureReason = std::string;

/** { ok: true, token } | { ok: false, reason } */
struct ServiceTokenResult {
    bool ok = false;
    std::string token;
    ServiceTokenFailureReason reason;
};

using ServiceTokenProvider = std::function<ServiceTokenResult()>;

struct ServiceTokenClientConfig {
    std::string baseUrl;
    std::string clientId;
    std::string clientSecret;
};

inline constexpr std::size_t kServiceTokenMaxResponseBytes = 64 * 1024;

struct ServiceTokenProviderOptions {
    std::string targetProjectKey;
    std::function<ServiceTokenClientConfig()> config;
    std::function<http::ClientResponse(const http::ClientRequest&)> fetchImpl;
    std::function<std::int64_t()> now;
};

/** createServiceTokenProvider。返す関数はスレッド安全。 */
ServiceTokenProvider create_service_token_provider(ServiceTokenProviderOptions options);

/** Excubitor が注入する env (CERNERE_BASE_URL / CERNERE_PROJECT_CLIENT_ID / CERNERE_PROJECT_CLIENT_SECRET) から読む。 */
ServiceTokenClientConfig service_token_client_config_from_env(const std::optional<std::map<std::string, std::string>>& env = std::nullopt);

/**
 * URL が HTTPS か、loopback (localhost / 127.0.0.1 / [::1]) の HTTP か。
 * 読めない URL は nullopt、http(s) 以外の scheme と loopback 以外の HTTP は false。
 * (TS の `new URL(...)` + protocol / hostname 検査と同じ判定。Voluptas の URL 検査も使う)
 */
std::optional<bool> is_secure_service_url(const std::string& text);

} // namespace di::cernere_service_token
