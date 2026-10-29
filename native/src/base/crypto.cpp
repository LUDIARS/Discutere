#include "base/crypto.hpp"

#include <memory>
#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace di::crypto {

namespace {
Bytes digest(const EVP_MD* md, std::string_view data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), out, &len, md, nullptr) != 1) throw std::runtime_error("digest failed");
    return Bytes(reinterpret_cast<const char*>(out), len);
}
} // namespace

Bytes sha256(std::string_view data) { return digest(EVP_sha256(), data); }
std::string sha256_hex(std::string_view data) { return hex_encode(sha256(data)); }
Bytes sha1(std::string_view data) { return digest(EVP_sha1(), data); }

Bytes hmac_sha256(std::string_view key, std::string_view data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(data.data()),
              data.size(), out, &len))
        throw std::runtime_error("hmac failed");
    return Bytes(reinterpret_cast<const char*>(out), len);
}

std::string hmac_sha256_hex(std::string_view key, std::string_view data) { return hex_encode(hmac_sha256(key, data)); }

Bytes random_bytes(std::size_t n) {
    Bytes out(n, '\0');
    if (n > 0 && RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(n)) != 1)
        throw std::runtime_error("RAND_bytes failed");
    return out;
}

std::string uuid_v4() {
    Bytes b = random_bytes(16);
    b[6] = static_cast<char>((static_cast<unsigned char>(b[6]) & 0x0F) | 0x40);
    b[8] = static_cast<char>((static_cast<unsigned char>(b[8]) & 0x3F) | 0x80);
    const std::string h = hex_encode(b);
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}

double random_unit() {
    const Bytes b = random_bytes(8);
    std::uint64_t v = 0;
    for (unsigned char c : b) v = (v << 8) | c;
    return static_cast<double>(v >> 11) / static_cast<double>(1ULL << 53);
}

bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string hex_encode(std::string_view bytes) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0xF]);
    }
    return out;
}

std::optional<Bytes> hex_decode(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Bytes out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = val(hex[i]);
        const int lo = val(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

std::string base64_encode(std::string_view bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::string base64url_encode(std::string_view bytes) {
    std::string s = base64_encode(bytes);
    for (auto& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

std::optional<Bytes> base64_decode(std::string_view text) {
    std::string s;
    s.reserve(text.size() + 3);
    for (char c : text) {
        if (c == '-') s.push_back('+');
        else if (c == '_') s.push_back('/');
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')
            s.push_back(c);
        else if (c == '\n' || c == '\r' || c == ' ') continue;
        else return std::nullopt;
    }
    std::size_t pad = 0;
    while (!s.empty() && s.back() == '=') {
        s.pop_back();
        ++pad;
    }
    if (pad > 2 || s.find('=') != std::string::npos) return std::nullopt;
    if (s.size() % 4 == 1) return std::nullopt;
    const std::size_t missing = (4 - s.size() % 4) % 4;
    s.append(missing, '=');
    Bytes out(3 * s.size() / 4, '\0');
    const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(s.data()), static_cast<int>(s.size()));
    if (n < 0) return std::nullopt;
    out.resize(static_cast<std::size_t>(n) - missing);
    return out;
}

bool ed25519_verify(std::string_view public_key_raw, std::string_view message, std::string_view signature) {
    if (public_key_raw.size() != 32 || signature.size() != 64) return false;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(public_key_raw.data()),
                                    public_key_raw.size()),
        &EVP_PKEY_free);
    if (!key) return false;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!ctx) return false;
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) return false;
    return EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
                            reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
}

} // namespace di::crypto
