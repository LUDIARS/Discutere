// cernere_service_token のテスト (1/3)。準備は cernere_service_token_support.hpp。
#include "cernere_service_token_support.hpp"

using namespace cernere_service_token_support;

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

DI_TEST_MAIN()
