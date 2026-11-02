#pragma once
// Cernere service token (PASETO v4.public, kind=service) の受け側検証 (src/cernere-service-token/service-token-verify.ts)。
//
// 照合するのは署名・`kind`・`exp`・`aud` (= 自分の storage_slug)・要求 scope だけで、
// 呼出元名 (`sub`) では分岐しない (Cernere spec/feature/service-token.md)。
// 判定の順序と理由 (reason) は TS と同じ:
//   503 audience_not_configured → 503 public_key_unavailable → 401 invalid_signature → 401 not_service_token
//   → 401 expired → 401 audience_mismatch → 403 scope_missing。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cernere_service_token/cernere_public_keys.hpp"

namespace di::cernere_service_token {

struct ServiceTokenClaims {
    std::string kind;  // "service"
    std::string sub;
    std::string aud;
    std::vector<std::string> scope;
    std::string exp;
};

struct ServiceTokenVerdict {
    bool ok = false;
    /** 401: 署名不正・期限切れ・aud 不一致・kind 不一致・形式不正 / 403: scope が無い / 503: 受け側の設定不足・公開鍵が取れない。 */
    int status = 0;
    std::string reason;
    std::optional<ServiceTokenClaims> claims;
};

struct VerifyServiceTokenInput {
    std::string token;
    std::string audience;
    std::string requiredScope;
    PublicKeyLoader loadKeys;
    std::int64_t nowMs = 0;
};

ServiceTokenVerdict verify_service_token(const VerifyServiceTokenInput& input);

} // namespace di::cernere_service_token
