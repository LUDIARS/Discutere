// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
#include "persona_bridge/authorization_assertion.hpp"

#include <algorithm>
#include <cmath>

#include "base/crypto.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"

namespace di::persona_bridge {

namespace {

constexpr std::size_t MAX_PAYLOAD_BYTES = 2'048;
constexpr std::size_t MAX_PAYLOAD_SEGMENT_LENGTH = (MAX_PAYLOAD_BYTES * 4 + 2) / 3;  // Math.ceil(2048 * 4 / 3)
constexpr std::size_t ED25519_SIGNATURE_SEGMENT_LENGTH = 86;

/** Ed25519 の SubjectPublicKeyInfo (DER) の固定前置き (RFC 8410)。 */
constexpr unsigned char kEd25519SpkiPrefix[] = {0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};

bool is_base64url(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

/** /^[0-9]{5,24}$/ */
bool is_snowflake(const std::string& s) {
    return s.size() >= 5 && s.size() <= 24 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

/** /^[A-Za-z0-9_-]{16,128}$/ */
bool is_jti(const std::string& s) { return s.size() >= 16 && s.size() <= 128 && is_base64url(s); }

std::string decode_segment(const std::string& value, const std::string& label) {
    if (!is_base64url(value)) throw PersonaBridgeAssertionVerificationError(label + " is not base64url");
    return node_base64url_decode(value);
}

/** SPKI DER (base64url) → 生の 32 バイト鍵。不正は設定エラー。 */
std::string assertion_public_key(const std::string& encoded) {
    const std::string der = is_base64url(encoded) ? node_base64url_decode(encoded) : std::string();
    if (der.size() != sizeof(kEd25519SpkiPrefix) + 32 ||
        !std::equal(std::begin(kEd25519SpkiPrefix), std::end(kEd25519SpkiPrefix), der.begin(),
                    [](unsigned char a, char b) { return a == static_cast<unsigned char>(b); }))
        throw PersonaBridgeAssertionConfigurationError(
            "DISCUTERE_PERSONA_BRIDGE_ASSERTION_PUBLIC_KEY must be an Ed25519 SPKI DER key encoded as base64url");
    return der.substr(sizeof(kEd25519SpkiPrefix));
}

Json assertion_payload(const std::string& segment) {
    const std::string bytes = decode_segment(segment, "assertion payload");
    if (bytes.empty() || bytes.size() > MAX_PAYLOAD_BYTES)
        throw PersonaBridgeAssertionVerificationError("assertion payload size is invalid");
    auto parsed = parse_json(str::sanitize_utf8(bytes), JsonLimits{MAX_PAYLOAD_BYTES * 3, 1'000, 16});
    if (!parsed || !parsed->is_object()) throw PersonaBridgeAssertionVerificationError("assertion payload is invalid JSON");
    return *parsed;
}

}  // namespace

std::string node_base64url_decode(const std::string& s) {
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else continue;
        acc = ((acc << 6) | static_cast<std::uint32_t>(v)) & 0xFFFFFF;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

VerifiedPersonaBridgeAssertion verify_persona_bridge_assertion(const std::string& compact, const std::string& public_key,
                                                               const std::string& author_id, std::int64_t now_ms) {
    if (str::trim(public_key).empty())
        throw PersonaBridgeAssertionConfigurationError("DISCUTERE_PERSONA_BRIDGE_ASSERTION_PUBLIC_KEY is required");
    const auto segments = str::split(compact, ".");
    if (segments.size() != 2 || segments[0].empty() || segments[1].empty())
        throw PersonaBridgeAssertionVerificationError("assertion format is invalid");
    const std::string& payload_segment = segments[0];
    const std::string& signature_segment = segments[1];
    // 署名の計算の前に大きさで拒否する。
    if (payload_segment.size() > MAX_PAYLOAD_SEGMENT_LENGTH || signature_segment.size() != ED25519_SIGNATURE_SEGMENT_LENGTH)
        throw PersonaBridgeAssertionVerificationError("assertion size is invalid");
    const std::string signature = decode_segment(signature_segment, "assertion signature");
    if (signature.size() != 64 || !crypto::ed25519_verify(assertion_public_key(public_key), payload_segment, signature))
        throw PersonaBridgeAssertionVerificationError("assertion signature is invalid");

    const Json payload = assertion_payload(payload_segment);
    const auto asserted = payload.find("authorId");
    if (asserted == payload.end() || !asserted->is_string() || !is_snowflake(asserted->get<std::string>()))
        throw PersonaBridgeAssertionVerificationError("assertion authorId is invalid");
    if (asserted->get<std::string>() != author_id)
        throw PersonaBridgeAssertionVerificationError("assertion authorId does not match the query");
    const auto aud = payload.find("aud");
    if (aud == payload.end() || !aud->is_string() || aud->get<std::string>() != PERSONA_BRIDGE_AUDIENCE)
        throw PersonaBridgeAssertionVerificationError("assertion audience is invalid");
    const auto exp = payload.find("exp");
    if (exp == payload.end() || !exp->is_number()) throw PersonaBridgeAssertionVerificationError("assertion exp is invalid");
    const double expires_at = exp->get<double>();
    if (!std::isfinite(expires_at) || std::trunc(expires_at) != expires_at)
        throw PersonaBridgeAssertionVerificationError("assertion exp is invalid");
    const auto jti = payload.find("jti");
    if (jti == payload.end() || !jti->is_string() || !is_jti(jti->get<std::string>()))
        throw PersonaBridgeAssertionVerificationError("assertion jti is invalid");
    const double now_seconds = std::floor(static_cast<double>(now_ms) / 1000.0);
    if (expires_at <= now_seconds || expires_at > now_seconds + static_cast<double>(MAX_ASSERTION_TTL_SECONDS))
        throw PersonaBridgeAssertionVerificationError("assertion is expired or not short-lived");
    return {asserted->get<std::string>(), static_cast<std::int64_t>(expires_at), jti->get<std::string>()};
}

} // namespace di::persona_bridge
