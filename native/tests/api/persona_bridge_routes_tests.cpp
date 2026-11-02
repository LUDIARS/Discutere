// w3-O: GET /persona-bridge/utterances (src/api/persona-bridge-routes.ts)。
// 移した TS テスト: tests/flow/persona-bridge.test.ts の route 部分 (token + 1 回限りの本人確認アサーション)。
// 境界: limit 1000/1001、snowflake 4/5/24/25 桁、巨大なアサーション、壊れた cursor、読み出し失敗で内部情報を出さない。
#include "w3o/w3o_support.hpp"

#include <set>

#include <openssl/evp.h>

#include "api/persona_bridge_routes.hpp"
#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "persona_bridge/authorization_assertion.hpp"

using namespace di;
using namespace di::api;

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
    /** Ed25519 の SPKI DER (固定 12 バイトの接頭辞 + 生 32 バイト) を base64url にしたもの。 */
    std::string spki_b64url() const {
        static const unsigned char prefix[] = {0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};
        return crypto::base64url_encode(std::string(reinterpret_cast<const char*>(prefix), sizeof(prefix)) + raw_public);
    }
};

const KeyPair& key() {
    static const KeyPair k;
    return k;
}

const std::int64_t NOW_MS = *time::parse_iso("2026-07-28T00:00:00.000Z");
const std::string kBridgeToken = "bridge-secret-0123456789abcdef0123456789";

std::string signed_assertion(const std::string& jti, const std::string& author_id = "123456789",
                             const std::string& audience = persona_bridge::PERSONA_BRIDGE_AUDIENCE,
                             std::int64_t expires_at = NOW_MS / 1000 + 120) {
    const std::string payload =
        crypto::base64url_encode(json_stringify(Json{{"authorId", author_id}, {"aud", audience}, {"exp", expires_at}, {"jti", jti}}));
    return payload + "." + crypto::base64url_encode(key().sign(payload));
}

http::Response get(const http::Router& r, const std::string& path, const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    http::Request req;
    req.method = "GET";
    req.target = path;
    const auto q = path.find('?');
    req.path = path.substr(0, q);
    if (q != std::string::npos) req.query_string = path.substr(q + 1);
    req.remote_address = "127.0.0.1";
    for (const auto& [k, v] : headers) req.headers.set(k, v);
    return r.handle(req);
}

std::vector<std::pair<std::string, std::string>> auth_headers(const std::string& jti) {
    return {{"authorization", "Bearer " + kBridgeToken}, {PERSONA_BRIDGE_ASSERTION_HEADER, signed_assertion(jti)}};
}

struct App {
    http::Router router;
    std::shared_ptr<Json> calls = std::make_shared<Json>(Json::array());
    std::shared_ptr<std::set<std::string>> consumed = std::make_shared<std::set<std::string>>();
    explicit App(PersonaBridgeUtteranceReader reader = {}) {
        PersonaBridgeRouteDeps deps;
        deps.token = [] { return kBridgeToken; };
        deps.assertionPublicKey = [] { return key().spki_b64url(); };
        deps.now = [] { return NOW_MS; };
        deps.serviceAudience = [] { return std::string("discutere"); };
        deps.loadServiceTokenKeys = [] { return std::vector<cernere_service_token::Ed25519PublicKey>{}; };
        auto consumed_set = consumed;
        deps.consumeAssertion = [consumed_set](const persona_bridge::VerifiedPersonaBridgeAssertion& a, std::int64_t) {
            return consumed_set->insert(a.jti).second;
        };
        auto c = calls;
        deps.readUtterances = reader ? reader : PersonaBridgeUtteranceReader([c](const PersonaBridgeUtteranceQuery& q) {
            c->push_back(Json{{"authorId", q.authorId}, {"postedAt", q.after.postedAt}, {"id", q.after.id}, {"limit", q.limit}});
            std::vector<persona_bridge::ExportableUtteranceRecord> rows{{"u1", "本人", "2026-07-28T00:00:00.000Z"},
                                                                         {"u2", "次頁", "2026-07-28T00:00:01.000Z"}};
            if (static_cast<std::int64_t>(rows.size()) > q.limit) rows.resize(static_cast<std::size_t>(q.limit));
            return rows;
        });
        router.mount("/api", persona_bridge_routes(deps));
    }
};

const std::string kPath = "/api/persona-bridge/utterances?authorId=123456789";

} // namespace

TEST_CASE("persona-bridge.test: token だけでは本人の発話を出さない (アサーション必須)") {
    App app;
    CHECK_EQ(get(app.router, kPath).status, 401);
    CHECK_EQ(get(app.router, kPath, {{"authorization", "Bearer " + kBridgeToken}}).status, 401);
    CHECK_EQ(app.calls->size(), 0u);
}

TEST_CASE("persona-bridge.test: 短い token は未設定 (503)、アサーション公開鍵が無ければ 503") {
    PersonaBridgeRouteDeps weak;
    weak.token = [] { return std::string("short"); };
    weak.readUtterances = [](const PersonaBridgeUtteranceQuery&) -> std::vector<persona_bridge::ExportableUtteranceRecord> {
        throw std::runtime_error("must not read");
    };
    weak.loadServiceTokenKeys = [] { return std::vector<cernere_service_token::Ed25519PublicKey>{}; };
    http::Router w;
    w.mount("/api", persona_bridge_routes(weak));
    CHECK_EQ(get(w, kPath, {{"authorization", "Bearer short"}}).status, 503);

    PersonaBridgeRouteDeps nokey = weak;
    nokey.token = [] { return kBridgeToken; };
    nokey.assertionPublicKey = [] { return std::string("  "); };
    http::Router n;
    n.mount("/api", persona_bridge_routes(nokey));
    CHECK_EQ(get(n, kPath, {{"authorization", "Bearer " + kBridgeToken}}).status, 503);
}

TEST_CASE("persona-bridge.test: 正常系は lookahead 1 件・内部 id を出さず・次頁の cursor を返す") {
    App app;
    const auto r = get(app.router, kPath + "&since=2026-07-27T00:00:00.000Z&limit=1", auth_headers("assertion-valid-0001"));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.headers.get("cache-control").value_or(""), std::string("private, no-store"));
    const auto body = parse_json_or_throw(r.body);
    CHECK_EQ(body["utterances"].size(), 1u);
    CHECK(!body["utterances"][0].contains("id"));
    const auto cursor = persona_bridge::decode_utterance_cursor(body["nextCursor"].get<std::string>());
    CHECK_EQ(cursor.postedAt, NOW_MS);
    CHECK_EQ(cursor.id, std::string("u1"));
    CHECK_EQ(app.calls->size(), 1u);
    CHECK_EQ((*app.calls)[0]["limit"].get<int>(), 2);
    CHECK_EQ((*app.calls)[0]["postedAt"].get<std::int64_t>(), *time::parse_iso("2026-07-27T00:00:00.000Z"));

    // ちょうど最後の頁なら nextCursor は null
    App exact([](const PersonaBridgeUtteranceQuery&) {
        return std::vector<persona_bridge::ExportableUtteranceRecord>{{"only", "最終頁", "2026-07-28T00:00:00.000Z"}};
    });
    const auto e = get(exact.router, kPath + "&limit=1", auth_headers("assertion-exact-page1"));
    CHECK_EQ(e.status, 200);
    CHECK(parse_json_or_throw(e.body)["nextCursor"].is_null());
}

TEST_CASE("persona-bridge.test: 再送・別人・別 aud・期限切れ・巨大なアサーションは 401") {
    App app;
    const auto h = auth_headers("assertion-replay-001");
    CHECK_EQ(get(app.router, kPath, h).status, 200);
    CHECK_EQ(get(app.router, kPath, h).status, 401);
    for (const auto& a : {signed_assertion("assertion-author-01", "987654321"),
                          signed_assertion("assertion-audience1", "123456789", "another-service"),
                          signed_assertion("assertion-expired-1", "123456789", persona_bridge::PERSONA_BRIDGE_AUDIENCE, NOW_MS / 1000)}) {
        CHECK_EQ(get(app.router, kPath, {{"authorization", "Bearer " + kBridgeToken}, {PERSONA_BRIDGE_ASSERTION_HEADER, a}}).status, 401);
    }
    CHECK_EQ(get(app.router, kPath,
                 {{"authorization", "Bearer " + kBridgeToken}, {PERSONA_BRIDGE_ASSERTION_HEADER, std::string(3000, 'A') + "." + std::string(86, 'A')}})
                 .status,
             401);
    CHECK_EQ(get(app.router, kPath, {{"authorization", "Bearer " + kBridgeToken}, {PERSONA_BRIDGE_ASSERTION_HEADER, std::string(1u << 20, 'A')}}).status,
             401);
}

TEST_CASE("境界: クエリ検証は 400 (limit 1000/1001・snowflake 桁数・since と cursor の併用・壊れた値)") {
    App app;
    int n = 0;
    auto jti = [&] { return "assertion-boundary-" + std::to_string(++n); };
    CHECK_EQ(get(app.router, kPath + "&limit=1000", auth_headers(jti())).status, 200);
    CHECK_EQ(get(app.router, kPath + "&limit=1001", {{"authorization", "Bearer " + kBridgeToken}}).status, 400);
    CHECK_EQ(get(app.router, kPath + "&limit=0", {{"authorization", "Bearer " + kBridgeToken}}).status, 400);
    CHECK_EQ(get(app.router, kPath + "&limit=1e3", {{"authorization", "Bearer " + kBridgeToken}}).status, 400);
    CHECK_EQ(get(app.router, kPath + "&limit=" + std::string(400, '9'), {{"authorization", "Bearer " + kBridgeToken}}).status, 400);
    CHECK_EQ(get(app.router, kPath + "&since=not-a-date", {{"authorization", "Bearer " + kBridgeToken}}).status, 400);
    CHECK_EQ(get(app.router, kPath + "&since=1&cursor=x", {{"authorization", "Bearer " + kBridgeToken}}).status, 400);
    CHECK_EQ(get(app.router, kPath + "&cursor=@@@", {{"authorization", "Bearer " + kBridgeToken}}).status, 400);
    CHECK_EQ(get(app.router, kPath + "&since=" + std::string(40, '9'), {{"authorization", "Bearer " + kBridgeToken}}).status, 400);
    CHECK_EQ(get(app.router, "/api/persona-bridge/utterances?authorId=1234", {{"authorization", "Bearer " + kBridgeToken}}).status, 400);
    CHECK_EQ(get(app.router, "/api/persona-bridge/utterances?authorId=" + std::string(25, '1'), {{"authorization", "Bearer " + kBridgeToken}}).status,
             400);
    CHECK_EQ(get(app.router, "/api/persona-bridge/utterances?authorId=12345a", {{"authorization", "Bearer " + kBridgeToken}}).status, 400);
    // 5 桁・24 桁はアサーション検証まで進む (authorId 不一致で 401)。
    CHECK_EQ(get(app.router, "/api/persona-bridge/utterances?authorId=12345", auth_headers(jti())).status, 401);
    CHECK_EQ(get(app.router, "/api/persona-bridge/utterances?authorId=" + std::string(24, '1'), auth_headers(jti())).status, 401);
}

TEST_CASE("persona-bridge.test: 読み出しの失敗は 500 + 定型文 (DB パスを出さない)") {
    App broken([](const PersonaBridgeUtteranceQuery&) -> std::vector<persona_bridge::ExportableUtteranceRecord> {
        throw std::runtime_error("SQLITE_CANTOPEN: unable to open database file /srv/discutere/kg.db");
    });
    const auto r = get(broken.router, kPath, auth_headers("assertion-reader-fail1"));
    CHECK_EQ(r.status, 500);
    CHECK_EQ(parse_json_or_throw(r.body)["error"].get<std::string>(), std::string("failed to read utterances"));
    CHECK(!str::contains(r.body, "/srv/"));
}

DI_TEST_MAIN()
