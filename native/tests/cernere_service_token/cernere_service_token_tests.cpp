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

namespace {

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

} // namespace

TEST_CASE("PASETO v4.public accepts tokens signed by the paseto library") {
    const auto keys = parse_well_known_keys(Json{{"keys", Json::array({Json{{"kid", "v1"}, {"alg", "EdDSA"}, {"public_key", FIXTURE_PUBLIC_KEY}, {"current", true}}})}});
    const Json payload = verify_paseto_v4_public(FIXTURE_TOKEN, keys);
    CHECK_EQ(payload["aud"].get<std::string>(), std::string("discutere"));
    CHECK_EQ(payload["exp"].get<std::string>(), std::string("2026-07-28T00:15:00.000Z"));
    std::string tampered = FIXTURE_TOKEN;
    tampered.replace(tampered.size() - 2, 2, "AA");
    CHECK_THROWS(verify_paseto_v4_public(tampered, keys));
}

TEST_CASE("PASETO: own keys, footer, tampering, wrong key, malformed") {
    KeyPair cernere, other;
    const std::vector<Ed25519PublicKey> keys{ed25519_public_key_from_raw(cernere.raw_public)};
    const std::string good = service_token(cernere, Json::object());
    CHECK_EQ(verify_paseto_v4_public(good, keys)["sub"].get<std::string>(), std::string("volputas"));
    // 期限切れ・aud 違いのトークンも署名としては通る (claims の検査は service-token-verify の責務)
    const auto expired = verify_paseto_v4_public(service_token(cernere, Json{{"exp", "2026-07-28T00:04:59.000Z"}}), keys);
    CHECK_EQ(expired["exp"].get<std::string>(), std::string("2026-07-28T00:04:59.000Z"));
    CHECK_EQ(verify_paseto_v4_public(service_token(cernere, Json{{"aud", "glab"}}), keys)["aud"].get<std::string>(), std::string("glab"));
    // footer は署名に含まれる
    const std::string with_footer = service_token(cernere, Json::object(), "{\"kid\":\"v1\"}");
    CHECK(verify_paseto_v4_public(with_footer, keys).is_object());
    const auto dot = with_footer.rfind('.');
    CHECK_THROWS(verify_paseto_v4_public(with_footer.substr(0, dot) + "." + crypto::base64url_encode("{\"kid\":\"v2\"}"), keys));
    CHECK_THROWS(verify_paseto_v4_public(with_footer.substr(0, dot), keys));  // footer を外すと署名が合わない
    // 別の発行者
    CHECK_THROWS(verify_paseto_v4_public(service_token(other, Json::object()), keys));
    // 本文の 1 文字を変える
    std::string flipped = good;
    flipped[20] = flipped[20] == 'A' ? 'B' : 'A';
    CHECK_THROWS(verify_paseto_v4_public(flipped, keys));
    // 鍵が複数 (ローテーション中) ならどれか 1 つで通る
    CHECK(verify_paseto_v4_public(good, {ed25519_public_key_from_raw(other.raw_public), keys[0]}).is_object());
    // 形式不正
    for (const char* bad : {"v4.local.abc", "v4.public.", "v4.public.a.b.c", "v4.public.ab+/", "v4.public.QUJD"})
        CHECK_THROWS(verify_paseto_v4_public(bad, keys));
    CHECK_THROWS(verify_paseto_v4_public(std::string("v4.public.") + std::string(kMaxPasetoTokenBytes, 'A'), keys));
    CHECK_THROWS(ed25519_public_key_from_raw("short"));
    // payload が object 以外は拒否 (署名は正しい)
    const std::string arr = "[1,2]";
    const std::string arr_token = "v4.public." + crypto::base64url_encode(arr + cernere.sign(pae({"v4.public.", arr, "", ""})));
    CHECK_THROWS(verify_paseto_v4_public(arr_token, keys));
    // PAE の書式
    CHECK_EQ(crypto::hex_encode(pae({"a"})), std::string("0100000000000000" "0100000000000000" "61"));
}

TEST_CASE("public key loader caches 10 minutes, backs off, needs a base URL") {
    KeyPair cernere;
    int fetches = 0;
    std::int64_t clock = 1'800'000'000'000;
    bool fail = false;
    auto loader = create_cernere_public_key_loader({[] { return std::string("http://127.0.0.1:9999/ignored/path"); },
                                                    [&](const http::ClientRequest& req) {
                                                        ++fetches;
                                                        CHECK_EQ(req.url, std::string("http://127.0.0.1:9999/.well-known/cernere-public-key"));
                                                        CHECK_EQ(req.max_redirects, 0);
                                                        if (fail) return json_response(Json::object(), 500);
                                                        return json_response(Json{{"keys", Json::array({Json{{"public_key", crypto::base64_encode(cernere.raw_public)}}})}});
                                                    },
                                                    [&] { return clock; }});
    CHECK_EQ(loader().size(), std::size_t{1});
    loader();
    CHECK_EQ(fetches, 1);
    clock += kPublicKeyCacheTtlMs;
    loader();
    CHECK_EQ(fetches, 2);
    clock += kPublicKeyCacheTtlMs;
    fail = true;
    CHECK_THROWS(loader());
    CHECK_THROWS(loader());  // 30 秒は取り直さない
    CHECK_EQ(fetches, 3);
    clock += kPublicKeyFailureBackoffMs;
    fail = false;
    CHECK_EQ(loader().size(), std::size_t{1});
    CHECK_EQ(fetches, 4);
    auto no_base = create_cernere_public_key_loader({[] { return std::string(""); }, {}, {}});
    try {
        no_base();
        CHECK(false);
    } catch (const std::exception& e) {
        CHECK(std::string(e.what()).find("CERNERE_BASE_URL") != std::string::npos);
    }
    CHECK_THROWS(parse_well_known_keys(Json::object()));
    CHECK(parse_well_known_keys(Json{{"keys", Json::array({Json{{"public_key", ""}}, Json{{"x", 1}}})}}).empty());
    CHECK_THROWS(parse_well_known_keys(Json{{"keys", Json::array({Json{{"public_key", "AAAA"}}})}}));
}

TEST_CASE("public key loader over real HTTP: response size limit") {
    auto router = std::make_shared<http::Router>();
    router->get("/.well-known/cernere-public-key", [](http::Request&) {
        return http::Response::json(Json{{"keys", Json::array()}, {"pad", std::string(kPublicKeyMaxResponseBytes, 'x')}});
    });
    http::ServerOptions so;
    so.port = 0;
    http::Server server(so, router);
    server.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(server.bound_port());
    auto loader = create_cernere_public_key_loader({[&] { return base; }, {}, {}});
    CHECK_THROWS(loader());  // 上限超過は失敗 (読み込まない)
    server.stop();
}

TEST_CASE("service token client caches until exp - 60s and reports failure reasons") {
    int issued = 0;
    std::int64_t clock = 1'800'000'000'000;
    std::vector<Json> bodies;
    std::vector<std::string> urls;
    auto provider = create_service_token_provider({"voluptas",
                                                   [] { return ServiceTokenClientConfig{"https://cernere.test", "client-1", "secret-1"}; },
                                                   [&](const http::ClientRequest& req) {
                                                       ++issued;
                                                       urls.push_back(req.url);
                                                       bodies.push_back(parse_json_or_throw(req.body));
                                                       CHECK_EQ(req.max_redirects, 0);
                                                       return json_response(Json{{"tokenType", "service"}, {"accessToken", "svc-token-" + std::to_string(issued)}, {"expiresIn", 900}});
                                                   },
                                                   [&] { return clock; }});
    CHECK_EQ(provider().token, std::string("svc-token-1"));
    clock += (900 - 61) * 1000;
    CHECK_EQ(provider().token, std::string("svc-token-1"));
    clock += 2 * 1000;
    CHECK_EQ(provider().token, std::string("svc-token-2"));
    CHECK_EQ(urls[0], std::string("https://cernere.test/api/auth/service-token"));
    CHECK_EQ(json_stringify(bodies[0]), std::string(R"({"client_id":"client-1","client_secret":"secret-1","target_project_key":"voluptas"})"));

    auto failure = [](ServiceTokenClientConfig cfg, std::function<http::ClientResponse(const http::ClientRequest&)> f) {
        return create_service_token_provider({"voluptas", [cfg] { return cfg; }, std::move(f), {}})().reason;
    };
    const ServiceTokenClientConfig creds{"https://cernere.test", "c", "s"};
    auto never = [](const http::ClientRequest&) -> http::ClientResponse { throw std::runtime_error("credentials must not be sent"); };
    CHECK_EQ(failure({creds.baseUrl, "c", ""}, never), std::string("credentials_missing"));
    CHECK_EQ(failure({"http://cernere.test", "c", "s"}, never), std::string("base_url_insecure"));
    CHECK_EQ(failure({"ftp://cernere.test", "c", "s"}, never), std::string("base_url_insecure"));
    CHECK_EQ(failure({"  ", "c", "s"}, never), std::string("base_url_missing"));
    CHECK_EQ(failure({"not a url", "c", "s"}, never), std::string("base_url_missing"));
    CHECK_EQ(failure({"http://localhost:7000", "c", "s"}, [](const http::ClientRequest&) { return json_response(Json::object(), 401); }),
             std::string("unauthorized"));
    CHECK_EQ(failure({"http://[::1]:7000", "c", "s"}, [](const http::ClientRequest&) { return json_response(Json::object(), 403); }),
             std::string("scope_undeclared"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) { return json_response(Json::object(), 404); }), std::string("target_not_found"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) { return json_response(Json::object(), 502); }), std::string("issuer_error"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) -> http::ClientResponse { throw std::runtime_error("fetch failed"); }),
             std::string("network"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) { return http::ClientResponse{}; }), std::string("network"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) {
                 http::ClientResponse r = json_response(Json::object());
                 r.body = "{bad";
                 return r;
             }),
             std::string("malformed_response"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) { return json_response(Json{{"accessToken", "x"}, {"expiresIn", 0}}); }),
             std::string("malformed_response"));
    CHECK_EQ(failure(creds, [](const http::ClientRequest&) { return json_response(Json{{"accessToken", ""}, {"expiresIn", 5}}); }),
             std::string("malformed_response"));
    const auto env_cfg = service_token_client_config_from_env(std::map<std::string, std::string>{{"CERNERE_BASE_URL", "https://c"}});
    CHECK_EQ(env_cfg.baseUrl, std::string("https://c"));
    CHECK(env_cfg.clientId.empty());
}

TEST_CASE("Voluptas bearer: service token first, fixed token only on failure, never logged") {
    using flow::resolve_voluptas_bearer;
    std::vector<std::string> logs;
    auto log = [&](const std::string& l) { logs.push_back(l); };
    CHECK_EQ(resolve_voluptas_bearer({ServiceTokenProvider([] { return ServiceTokenResult{true, "svc", ""}; }), "fixed", log}),
             std::string("svc"));
    CHECK(logs.empty());
    CHECK_EQ(resolve_voluptas_bearer({ServiceTokenProvider([] { return ServiceTokenResult{false, "", "scope_undeclared"}; }),
                                      "fixed-secret-value", log}),
             std::string("fixed-secret-value"));
    CHECK_EQ(logs.size(), std::size_t{1});
    CHECK(logs[0].find("reason=scope_undeclared") != std::string::npos);
    CHECK(logs[0].find("fixed-secret-value") == std::string::npos);
    try {
        resolve_voluptas_bearer({ServiceTokenProvider([] { return ServiceTokenResult{false, "", "network"}; }), "", log});
        CHECK(false);
    } catch (const std::exception& e) {
        CHECK(std::string(e.what()).find("reason=network") != std::string::npos);
    }
    CHECK_THROWS(resolve_voluptas_bearer({std::nullopt, "", log}));
    CHECK_EQ(resolve_voluptas_bearer({std::nullopt, "fixed", log}), std::string("fixed"));
}

TEST_CASE("Voluptas pull: URL checks, paging with cursor, redirects refused, page limit") {
    using namespace di::flow;
    std::vector<std::string> auth, urls;
    int page = 0;
    PullVoluptasArgs a;
    a.url = "https://voluptas.test/api/personas/export?limit=5&cursor=old";
    a.token = "fixed";
    a.serviceToken = ServiceTokenProvider([] { return ServiceTokenResult{true, "svc-token", ""}; });
    a.fetchImpl = [&](const http::ClientRequest& req) {
        auth.push_back(*req.headers.get("authorization"));
        urls.push_back(req.url);
        CHECK_EQ(req.max_redirects, 0);
        ++page;
        http::ClientResponse r;
        r.transport_ok = true;
        r.status = 200;
        r.body = page == 1 ? "{\"pseudoId\":\"a\"}\n{bad\n" : "[{\"pseudoId\":\"b\"},{\"pseudoId\":\"c\"}]";
        if (page == 1) r.headers.set("x-next-cursor", "c 2&x");
        return r;
    };
    const auto r = pull_voluptas_personas(a);
    CHECK_EQ(r.pages, 2);
    CHECK_EQ(r.personas.size(), std::size_t{3});
    CHECK_EQ(r.invalidJsonLines, 1);
    CHECK_EQ(auth, (std::vector<std::string>{"Bearer svc-token", "Bearer svc-token"}));
    CHECK_EQ(urls[0], a.url);
    CHECK_EQ(urls[1], std::string("https://voluptas.test/api/personas/export?limit=5&cursor=c+2%26x"));

    for (const char* bad : {"http://voluptas.test/x", "ftp://voluptas.test/x", "http://10.0.0.1/x"}) {
        PullVoluptasArgs b = a;
        b.url = bad;
        CHECK_THROWS(pull_voluptas_personas(b));
    }
    PullVoluptasArgs loop = a;
    loop.url = "http://127.0.0.1:4000/export";
    loop.fetchImpl = [](const http::ClientRequest&) {
        http::ClientResponse r;
        r.transport_ok = true;
        r.status = 200;
        r.headers.set("x-next-cursor", "again");
        return r;
    };
    try {
        pull_voluptas_personas(loop);
        CHECK(false);
    } catch (const std::exception& e) {
        CHECK(std::string(e.what()).find("page limit") != std::string::npos);
    }
    PullVoluptasArgs redirect = a;
    redirect.url = "http://localhost:4000/export";
    redirect.fetchImpl = [](const http::ClientRequest&) {
        http::ClientResponse r;
        r.transport_ok = true;
        r.status = 302;
        r.headers.set("location", "https://elsewhere.test/");
        return r;
    };
    CHECK_THROWS(pull_voluptas_personas(redirect));
}

DI_TEST_MAIN()
