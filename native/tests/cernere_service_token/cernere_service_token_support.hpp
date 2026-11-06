#pragma once
// cernere_service_token_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// src/cernere-service-token/{paseto-v4-public,cernere-public-keys,service-token-client}.ts と
// src/flow/voluptas-persona-client.ts の移植の確認 (tests/flow/cernere-service-token.test.ts の観点)。
// テストの鍵はテスト内で OpenSSL (EVP) で作って署名する (署名アルゴリズムは自作しない)。
#include "check.hpp"

#include <openssl/evp.h>

#include <memory>

#include "base/crypto.hpp"
#include "base/json.hpp"
#include "cernere_service_token/cernere_public_keys.hpp"
#include "cernere_service_token/paseto_v4_public.hpp"
#include "cernere_service_token/service_token_client.hpp"
#include "flow/voluptas_persona_client.hpp"
#include "infra/http_server.hpp"

using namespace di;
using namespace di::cernere_service_token;

namespace cernere_service_token_support {

// Cernere と同じ paseto ライブラリ (V4.sign) で作った token (seed=0x07*32, kid=v1)。TS テストの固定値。
const char* FIXTURE_PUBLIC_KEY = "6kpsY+KcUgq+9VB7Ey7F+ZVHdq6+vnuSQh7qaRRG0iw=";
const char* FIXTURE_TOKEN =
    "v4.public.eyJraW5kIjoic2VydmljZSIsInN1YiI6InZvbHB1dGFzIiwiYXVkIjoiZGlzY3V0ZXJlIiwic2NvcGUiOlsicGVyc29uYS1icmlkZ2U6d3JpdGUiXSwiaWF0IjoiMjAyNi0wNy0yOFQwMDowMDowMC4wMDBaIiwiZXhwIjoiMjAyNi0wNy0yOFQwMDoxNTowMC4wMDBaIiwianRpIjoiZml4dHVyZS0wMDAxIiwia2lkIjoidjEifW4L7tWdrGRscubWbovf5pAXsHHgM85L4pYlgsyEXrNx4Fusewx0hq-dGVdPinvbkRbxjhl9uYtT9ryA6apZCwM";

struct KeyPair {
    std::shared_ptr<EVP_PKEY> pkey;
    std::string raw_public;

    KeyPair() {
        EVP_PKEY* k = nullptr;
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_keygen(ctx, &k) <= 0) throw std::runtime_error("keygen");
        EVP_PKEY_CTX_free(ctx);
        pkey.reset(k, EVP_PKEY_free);
        std::size_t len = 32;
        raw_public.assign(32, '\0');
        EVP_PKEY_get_raw_public_key(k, reinterpret_cast<unsigned char*>(raw_public.data()), &len);
    }

    std::string sign(const std::string& message) const {
        EVP_MD_CTX* md = EVP_MD_CTX_new();
        std::string sig(64, '\0');
        std::size_t len = sig.size();
        if (EVP_DigestSignInit(md, nullptr, nullptr, nullptr, pkey.get()) <= 0 ||
            EVP_DigestSign(md, reinterpret_cast<unsigned char*>(sig.data()), &len,
                           reinterpret_cast<const unsigned char*>(message.data()), message.size()) <= 0)
            throw std::runtime_error("sign");
        EVP_MD_CTX_free(md);
        return sig;
    }
};

std::string service_token(const KeyPair& key, Json claims, const std::string& footer = "") {
    Json base = Json{{"kind", "service"}, {"sub", "volputas"}, {"aud", "discutere"}, {"scope", Json::array({"persona-bridge:write"})},
                     {"iat", "2026-07-28T00:00:00.000Z"}, {"exp", "2026-07-28T00:15:00.000Z"}, {"jti", "svc-0001"}};
    for (auto it = claims.begin(); it != claims.end(); ++it) base[it.key()] = it.value();
    const std::string message = json_stringify(base);
    const std::string sig = key.sign(pae({"v4.public.", message, footer, ""}));
    std::string token = "v4.public." + crypto::base64url_encode(message + sig);
    if (!footer.empty()) token += "." + crypto::base64url_encode(footer);
    return token;
}

http::ClientResponse json_response(const Json& body, int status = 200) {
    http::ClientResponse r;
    r.transport_ok = true;
    r.status = status;
    r.body = json_stringify(body);
    return r;
}

} // namespace cernere_service_token_support
