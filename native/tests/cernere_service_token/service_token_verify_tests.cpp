// w2-J: service-token-verify (exp/aud/kind/scope/署名) と persona-bridge の bridge-auth・再送防止の境界。
// tests/flow/cernere-service-token.test.ts の検証部分と tests/flow/persona-bridge.test.ts の nonce 部分の移植。
// 鍵はテスト内で OpenSSL (EVP) で作る (署名アルゴリズムは自作しない)。
#include "check.hpp"

#include <openssl/evp.h>

#include <atomic>
#include <memory>

#include "base/crypto.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "cernere_service_token/paseto_v4_public.hpp"
#include "cernere_service_token/service_token_verify.hpp"
#include "persona_bridge/assertion_replay_store.hpp"
#include "persona_bridge/bridge_auth.hpp"

using namespace di;
using namespace di::cernere_service_token;

namespace {

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
            EVP_DigestSign(md, reinterpret_cast<unsigned char*>(sig.data()), &len, reinterpret_cast<const unsigned char*>(message.data()),
                           message.size()) <= 0)
            throw std::runtime_error("sign");
        EVP_MD_CTX_free(md);
        return sig;
    }
};

const KeyPair& cernere() {
    static KeyPair k;
    return k;
}
const KeyPair& other_issuer() {
    static KeyPair k;
    return k;
}

const std::int64_t NOW_MS = *time::parse_iso("2026-07-28T00:05:00.000Z");
const std::string AUDIENCE = "discutere";

std::string service_token(Json claims, const KeyPair& key = cernere()) {
    Json base{{"kind", "service"}, {"sub", "volputas"}, {"aud", AUDIENCE}, {"scope", Json::array({"persona-bridge:write"})},
              {"iat", "2026-07-28T00:00:00.000Z"}, {"exp", "2026-07-28T00:15:00.000Z"}, {"jti", "svc-0001"}};
    for (auto it = claims.begin(); it != claims.end(); ++it) {
        if (it.value().is_null()) base.erase(it.key());
        else base[it.key()] = it.value();
    }
    const std::string message = json_stringify(base);
    return "v4.public." + crypto::base64url_encode(message + key.sign(pae({"v4.public.", message, "", ""})));
}

PublicKeyLoader loader() {
    return [] { return std::vector<Ed25519PublicKey>{ed25519_public_key_from_raw(cernere().raw_public)}; };
}

ServiceTokenVerdict verdict_of(const std::string& token, std::string scope = "persona-bridge:write", std::int64_t now = NOW_MS) {
    return verify_service_token({token, AUDIENCE, std::move(scope), loader(), now});
}

void expect(const ServiceTokenVerdict& v, int status, const std::string& reason) {
    CHECK(!v.ok);
    CHECK_EQ(v.status, status);
    CHECK_EQ(v.reason, reason);
}

} // namespace

TEST_CASE("verify_service_token: 署名・kind・exp・aud・scope (TS と同じ判定)") {
    const auto ok = verdict_of(service_token(Json::object()));
    CHECK(ok.ok && ok.claims && ok.claims->sub == "volputas");
    expect(verdict_of(service_token({{"aud", "glab"}})), 401, "audience_mismatch");
    expect(verdict_of(service_token({{"exp", "2026-07-28T00:04:59.000Z"}})), 401, "expired");
    expect(verdict_of(service_token({{"kind", "user_for_project"}})), 401, "not_service_token");
    expect(verdict_of(service_token(Json::object(), other_issuer())), 401, "invalid_signature");
    expect(verdict_of(service_token({{"scope", Json::array({"persona-export:read"})}})), 403, "scope_missing");
    CHECK(verdict_of(service_token({{"sub", "some-other-service"}})).ok);
    expect(verify_service_token({service_token(Json::object()), "", "persona-bridge:write", loader(), NOW_MS}), 503, "audience_not_configured");
    expect(verify_service_token({service_token(Json::object()), "  ", "persona-bridge:write", loader(), NOW_MS}), 503, "audience_not_configured");
}

TEST_CASE("verify_service_token: 境界 (期限ちょうど・不正な exp・型違いの claims・改ざん・鍵が取れない・巨大な token)") {
    const auto token = service_token(Json::object());
    const std::int64_t exp = *time::parse_iso("2026-07-28T00:15:00.000Z");
    CHECK(verdict_of(token, "persona-bridge:write", exp - 1).ok);
    expect(verdict_of(token, "persona-bridge:write", exp), 401, "expired");  // exp <= now
    expect(verdict_of(service_token({{"exp", "not-a-date"}})), 401, "expired");
    expect(verdict_of(service_token({{"exp", 1'900'000'000'000}})), 401, "not_service_token");
    expect(verdict_of(service_token({{"sub", ""}})), 401, "not_service_token");
    expect(verdict_of(service_token({{"sub", nullptr}})), 401, "not_service_token");
    expect(verdict_of(service_token({{"scope", "persona-bridge:write"}})), 401, "not_service_token");
    expect(verdict_of(service_token({{"scope", Json::array({"persona-bridge:write", 3})}})), 401, "not_service_token");
    expect(verdict_of(service_token({{"aud", Json::array({"discutere"})}})), 401, "not_service_token");
    // 1 文字の改ざん
    std::string tampered = token;
    tampered[20] = tampered[20] == 'A' ? 'B' : 'A';
    expect(verdict_of(tampered), 401, "invalid_signature");
    expect(verdict_of(token.substr(0, token.size() - 2) + "AA"), 401, "invalid_signature");
    expect(verdict_of("v4.public." + std::string(64 * 1024, 'A')), 401, "invalid_signature");
    expect(verdict_of(""), 401, "invalid_signature");
    expect(verify_service_token({token, AUDIENCE, "persona-bridge:write", [] () -> std::vector<Ed25519PublicKey> { throw std::runtime_error("down"); }, NOW_MS}),
           503, "public_key_unavailable");
    expect(verify_service_token({token, AUDIENCE, "persona-bridge:write", nullptr, NOW_MS}), 503, "public_key_unavailable");
    expect(verify_service_token({token, AUDIENCE, "persona-bridge:write", [] { return std::vector<Ed25519PublicKey>{}; }, NOW_MS}), 401,
           "invalid_signature");
}

namespace {
persona_bridge::BridgeAuthOptions bridge_options(std::string fixed, std::string audience = AUDIENCE) {
    persona_bridge::BridgeAuthOptions o;
    o.token = [fixed] { return fixed; };
    o.serviceAudience = [audience] { return audience; };
    o.loadServiceTokenKeys = loader();
    o.now = [] { return NOW_MS; };
    o.label = "persona-bridge";
    return o;
}
const std::string BRIDGE_TOKEN = "bridge-secret-0123456789abcdef0123456789";  // 明らかな偽物
} // namespace

TEST_CASE("bearer_token: /^Bearer ([^\\s]+)$/") {
    using persona_bridge::bearer_token;
    CHECK_EQ(bearer_token(std::string("Bearer abc")), std::string("abc"));
    CHECK_EQ(bearer_token(std::nullopt), std::string(""));
    CHECK_EQ(bearer_token(std::string("bearer abc")), std::string(""));
    CHECK_EQ(bearer_token(std::string("Bearer  abc")), std::string(""));
    CHECK_EQ(bearer_token(std::string("Bearer abc def")), std::string(""));
    CHECK_EQ(bearer_token(std::string("Bearer abc\t")), std::string(""));
    CHECK_EQ(bearer_token(std::string("Bearer ")), std::string(""));
    CHECK_EQ(bearer_token(std::string("Basic abc")), std::string(""));
}

TEST_CASE("authorize_bridge_request: service token と固定トークン・503 default-deny・短いトークン") {
    using persona_bridge::authorize_bridge_request;
    std::vector<std::string> logs;
    const int sink = log::add_sink([&](const log::Record& r) { logs.push_back(r.message); });
    auto auth = [](const std::string& bearer, const persona_bridge::BridgeAuthOptions& o) { return authorize_bridge_request("Bearer " + bearer, o); };
    const auto o = bridge_options(BRIDGE_TOKEN);
    CHECK(auth(service_token(Json::object()), o).ok);
    CHECK(auth(BRIDGE_TOKEN, o).ok);
    auto r = auth(service_token({{"scope", Json::array()}}), o);
    CHECK(!r.ok && r.status == 403 && r.error == "forbidden");
    r = auth(service_token({{"aud", "volputas"}}), o);
    CHECK(!r.ok && r.status == 401 && r.error == "unauthorized");
    CHECK_EQ(auth(service_token(Json::object(), other_issuer()), o).status, 401);
    CHECK_EQ(auth("wrong-fixed-token-0123456789abcdef0123", o).status, 401);
    CHECK_EQ(auth(BRIDGE_TOKEN.substr(0, BRIDGE_TOKEN.size() - 1), o).status, 401);  // 長さ違い
    CHECK_EQ(auth(BRIDGE_TOKEN + "x", o).status, 401);
    CHECK_EQ(authorize_bridge_request(std::nullopt, o).status, 401);
    // 固定トークン未設定でも service token は通る。固定トークン経路は 503
    const auto service_only = bridge_options("");
    CHECK(auth(service_token(Json::object()), service_only).ok);
    r = auth("any-fixed-token-0123456789abcdef01234", service_only);
    CHECK(!r.ok && r.status == 503);
    CHECK(!str::contains(r.error, "any-fixed"));
    // 31 文字 (上限-1) は未設定扱い、32 文字ちょうどは設定済み
    const std::string t31(31, 'k'), t32(32, 'k');
    CHECK_EQ(auth(t31, bridge_options(t31)).status, 503);
    CHECK(auth(t32, bridge_options(t32)).ok);
    CHECK_EQ(authorize_bridge_request(std::nullopt, bridge_options(t31)).status, 503);
    // aud 未設定なら service token は判定できないので 503 (fail-closed)、ログに token を出さない
    logs.clear();
    const auto token = service_token(Json::object());
    r = auth(token, bridge_options(BRIDGE_TOKEN, ""));
    CHECK(!r.ok && r.status == 503);
    CHECK_EQ(logs.size(), std::size_t{1});
    CHECK(str::contains(logs[0], "reason=audience_not_configured"));
    for (const auto& l : logs) CHECK(!str::contains(l, token.substr(10, 20)) && !str::contains(l, BRIDGE_TOKEN));
    log::remove_sink(sink);
}

TEST_CASE("assertion replay store: 同じ jti は 1 回だけ・期限切れの nonce は消す") {
    auto db = sql::Database::open(":memory:");
    db->exec("CREATE TABLE persona_bridge_assertion_nonce (jti TEXT PRIMARY KEY, expires_at INTEGER NOT NULL)");
    const std::int64_t now_s = NOW_MS / 1000;
    persona_bridge::VerifiedPersonaBridgeAssertion a{"123456789", now_s + 120, "assertion-persist-001"};
    CHECK(persona_bridge::consume_persona_bridge_assertion(*db, a, NOW_MS));
    CHECK(!persona_bridge::consume_persona_bridge_assertion(*db, a, NOW_MS));
    // 期限 (expires_at <= now) を過ぎた nonce は消えるので、同じ jti でも期限後は再び 1 回受ける
    CHECK(persona_bridge::consume_persona_bridge_assertion(*db, a, (now_s + 120) * 1000));
    persona_bridge::VerifiedPersonaBridgeAssertion b{"123456789", now_s + 60, "other"};
    CHECK(persona_bridge::consume_persona_bridge_assertion(*db, b, NOW_MS));
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM persona_bridge_assertion_nonce")->i64("n"), 2);
}

DI_TEST_MAIN()
