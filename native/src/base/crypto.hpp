#pragma once
// 暗号の基本処理はすべて OpenSSL 3 に任せる (自作しない)。
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace di::crypto {

using Bytes = std::string;  // バイト列 (中身は任意のバイト)

Bytes sha256(std::string_view data);
std::string sha256_hex(std::string_view data);
Bytes sha1(std::string_view data);
Bytes hmac_sha256(std::string_view key, std::string_view data);
std::string hmac_sha256_hex(std::string_view key, std::string_view data);

/** 暗号論的乱数 (RAND_bytes)。失敗は std::runtime_error。 */
Bytes random_bytes(std::size_t n);
/** uuid v4 (小文字, ハイフン付き)。uuid パッケージ / crypto.randomUUID と同じ形。 */
std::string uuid_v4();
/** [0, 1) の一様乱数 (Math.random の代わり。乱数源は RAND_bytes)。 */
double random_unit();

/** 長さが違えば false。同じ長さなら CRYPTO_memcmp で定数時間比較 (timingSafeEqual 相当)。 */
bool constant_time_equal(std::string_view a, std::string_view b);

std::string hex_encode(std::string_view bytes);
std::optional<Bytes> hex_decode(std::string_view hex);
std::string base64_encode(std::string_view bytes);
std::string base64url_encode(std::string_view bytes);  // パディング無し
/** 標準 base64 / base64url の両方を受ける (パディング有無とも)。不正は nullopt。 */
std::optional<Bytes> base64_decode(std::string_view text);

/** Ed25519 署名検証 (公開鍵は生 32 バイト、署名は 64 バイト)。 */
bool ed25519_verify(std::string_view public_key_raw, std::string_view message, std::string_view signature);

} // namespace di::crypto
