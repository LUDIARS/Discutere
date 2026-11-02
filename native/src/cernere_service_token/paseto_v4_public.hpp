#pragma once
// src/cernere-service-token/paseto-v4-public.ts の移植: PASETO v4.public (Ed25519) の署名検証だけを行う最小実装。
// 仕様 (PAE + Ed25519) を OpenSSL (di::crypto::ed25519_verify) で直接たどる。claims の意味 (kind / aud / exp / scope)
// はここでは見ない (service-token-verify の責務)。
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace di::cernere_service_token {

class PasetoFormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Ed25519 公開鍵 (生 32 バイト)。 */
struct Ed25519PublicKey {
    std::string raw;
};

bool is_paseto_v4_public(std::string_view token);
/** 32 バイトでなければ PasetoFormatError。 */
Ed25519PublicKey ed25519_public_key_from_raw(std::string_view raw);
/** Pre-Authentication Encoding (PASETO 共通)。 */
std::string pae(const std::vector<std::string>& pieces);

/** token の上限 (HTTP ヘッダ 1 本に収まる大きさ)。超えたら形式不正。 */
inline constexpr std::size_t kMaxPasetoTokenBytes = 16 * 1024;

/**
 * 署名が鍵のどれか 1 つで通れば payload (JSON object) を返す。
 * 形式不正・署名不一致は PasetoFormatError、payload が JSON でなければ std::runtime_error。
 * implicit assertion は Cernere と同じく空。
 */
Json verify_paseto_v4_public(std::string_view token, const std::vector<Ed25519PublicKey>& keys);

} // namespace di::cernere_service_token
