#include "persona_bridge/bridge_auth.hpp"

#include "base/crypto.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "cernere_service_token/paseto_v4_public.hpp"
#include "cernere_service_token/service_token_verify.hpp"

namespace di::persona_bridge {

std::string bearer_token(const std::optional<std::string>& header) {
    if (!header) return "";
    const std::string& h = *header;
    constexpr std::string_view prefix = "Bearer ";
    if (!str::starts_with(h, prefix) || h.size() == prefix.size()) return "";
    // Node はヘッダを latin1 で文字列にするので、\s に当たるのは 1 バイトの 09-0D / 20 / A0 だけ。
    for (std::size_t i = prefix.size(); i < h.size(); ++i) {
        const auto c = static_cast<unsigned char>(h[i]);
        if ((c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0xA0) return "";
    }
    return h.substr(prefix.size());
}

BridgeAuthResult authorize_bridge_request(const std::optional<std::string>& header, const BridgeAuthOptions& opts) {
    const std::string presented = bearer_token(header);
    if (cernere_service_token::is_paseto_v4_public(presented)) {
        cernere_service_token::VerifyServiceTokenInput in;
        in.token = presented;
        in.audience = opts.serviceAudience ? opts.serviceAudience() : std::string();
        in.requiredScope = PERSONA_BRIDGE_SERVICE_SCOPE;
        in.loadKeys = opts.loadServiceTokenKeys;
        in.nowMs = opts.now ? opts.now() : 0;
        const auto verdict = cernere_service_token::verify_service_token(in);
        if (verdict.ok) return {true, 0, ""};
        if (verdict.status == 503) {
            log::warn("[" + opts.label + "] service token verification unavailable (reason=" + verdict.reason + ")");
            return {false, 503, "persona bridge service token verification is not configured"};
        }
        if (verdict.status == 403) return {false, 403, "forbidden"};
        return {false, 401, "unauthorized"};
    }
    const std::string expected = opts.token ? opts.token() : std::string();
    if (str::js_length(expected) < MIN_BRIDGE_TOKEN_LENGTH) {
        return {false, 503,
                "persona bridge is not configured (DISCUTERE_PERSONA_BRIDGE_TOKEN must be at least " + std::to_string(MIN_BRIDGE_TOKEN_LENGTH) +
                    " characters)"};
    }
    if (!crypto::constant_time_equal(presented, expected)) return {false, 401, "unauthorized"};
    return {true, 0, ""};
}

} // namespace di::persona_bridge
