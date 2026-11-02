#include "cernere_service_token/paseto_v4_public.hpp"

#include <cstdint>

#include "base/crypto.hpp"

namespace di::cernere_service_token {

namespace {

constexpr std::string_view HEADER = "v4.public.";
constexpr std::size_t SIGNATURE_BYTES = 64;

std::string le64(std::uint64_t value) {
    value &= 0x7fffffffffffffffULL;
    std::string out(8, '\0');
    for (int i = 0; i < 8; ++i) out[static_cast<std::size_t>(i)] = static_cast<char>((value >> (8 * i)) & 0xff);
    return out;
}

std::string decode_base64url(std::string_view value) {
    for (char c : value) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) throw PasetoFormatError("invalid base64url segment");
    }
    if (value.empty()) return {};
    auto decoded = crypto::base64_decode(value);
    if (!decoded) throw PasetoFormatError("invalid base64url segment");
    return *decoded;
}

} // namespace

bool is_paseto_v4_public(std::string_view token) { return token.substr(0, HEADER.size()) == HEADER; }

Ed25519PublicKey ed25519_public_key_from_raw(std::string_view raw) {
    if (raw.size() != 32) throw PasetoFormatError("Ed25519 public key must be 32 bytes");
    return Ed25519PublicKey{std::string(raw)};
}

std::string pae(const std::vector<std::string>& pieces) {
    std::string out = le64(pieces.size());
    for (const auto& p : pieces) {
        out += le64(p.size());
        out += p;
    }
    return out;
}

Json verify_paseto_v4_public(std::string_view token, const std::vector<Ed25519PublicKey>& keys) {
    if (!is_paseto_v4_public(token)) throw PasetoFormatError("not a v4.public token");
    if (token.size() > kMaxPasetoTokenBytes) throw PasetoFormatError("malformed v4.public token");
    const std::string_view rest = token.substr(HEADER.size());
    // split(".") の個数は 1 か 2
    const auto dot = rest.find('.');
    const std::string_view body_part = rest.substr(0, dot);
    const bool has_footer = dot != std::string_view::npos;
    const std::string_view footer_part = has_footer ? rest.substr(dot + 1) : std::string_view{};
    if (body_part.empty() || (has_footer && footer_part.find('.') != std::string_view::npos))
        throw PasetoFormatError("malformed v4.public token");
    const std::string body = decode_base64url(body_part);
    if (body.size() <= SIGNATURE_BYTES) throw PasetoFormatError("v4.public token is too short");
    const std::string footer = has_footer ? decode_base64url(footer_part) : std::string();
    const std::string message = body.substr(0, body.size() - SIGNATURE_BYTES);
    const std::string signature = body.substr(body.size() - SIGNATURE_BYTES);
    const std::string signed_bytes = pae({std::string(HEADER), message, footer, std::string()});
    bool valid = false;
    for (const auto& key : keys) {
        if (crypto::ed25519_verify(key.raw, signed_bytes, signature)) {
            valid = true;
            break;
        }
    }
    if (!valid) throw PasetoFormatError("signature verification failed");
    std::string error;
    auto payload = parse_json(message, JsonLimits{kMaxPasetoTokenBytes, 10'000, 16}, &error);
    if (!payload) throw std::runtime_error("payload is not valid JSON: " + error);
    if (!payload->is_object()) throw PasetoFormatError("payload must be a JSON object");
    return *payload;
}

} // namespace di::cernere_service_token
