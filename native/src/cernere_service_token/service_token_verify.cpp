#include "cernere_service_token/service_token_verify.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "base/time.hpp"

namespace di::cernere_service_token {

namespace {

std::optional<ServiceTokenClaims> read_claims(const Json& payload) {
    if (!payload.is_object()) return std::nullopt;
    auto str_at = [&](const char* k) -> const Json* {
        auto it = payload.find(k);
        return it != payload.end() && it->is_string() ? &*it : nullptr;
    };
    const Json* kind = str_at("kind");
    if (!kind || kind->get<std::string>() != "service") return std::nullopt;
    const Json* sub = str_at("sub");
    if (!sub || sub->get<std::string>().empty()) return std::nullopt;
    const Json* aud = str_at("aud");
    const Json* exp = str_at("exp");
    if (!aud || !exp) return std::nullopt;
    auto scope_it = payload.find("scope");
    if (scope_it == payload.end() || !scope_it->is_array()) return std::nullopt;
    ServiceTokenClaims c;
    for (const auto& v : *scope_it) {
        if (!v.is_string()) return std::nullopt;
        c.scope.push_back(v.get<std::string>());
    }
    c.kind = "service";
    c.sub = sub->get<std::string>();
    c.aud = aud->get<std::string>();
    c.exp = exp->get<std::string>();
    return c;
}

ServiceTokenVerdict deny(int status, std::string reason) {
    ServiceTokenVerdict v;
    v.status = status;
    v.reason = std::move(reason);
    return v;
}

} // namespace

ServiceTokenVerdict verify_service_token(const VerifyServiceTokenInput& input) {
    if (str::trim(input.audience).empty()) return deny(503, "audience_not_configured");
    std::vector<Ed25519PublicKey> keys;
    try {
        if (!input.loadKeys) throw std::runtime_error("no loader");
        keys = input.loadKeys();
    } catch (...) {
        return deny(503, "public_key_unavailable");
    }
    Json payload;
    try {
        payload = verify_paseto_v4_public(input.token, keys);
    } catch (...) {
        return deny(401, "invalid_signature");
    }
    auto claims = read_claims(payload);
    if (!claims) return deny(401, "not_service_token");
    // Date.parse が読めない (NaN) 値は期限切れと同じ扱い。
    const auto expires_at = time::parse_iso(claims->exp);
    if (!expires_at || *expires_at <= input.nowMs) return deny(401, "expired");
    if (claims->aud != input.audience) return deny(401, "audience_mismatch");
    if (std::find(claims->scope.begin(), claims->scope.end(), input.requiredScope) == claims->scope.end()) return deny(403, "scope_missing");
    ServiceTokenVerdict ok;
    ok.ok = true;
    ok.claims = std::move(claims);
    return ok;
}

} // namespace di::cernere_service_token
