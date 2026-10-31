#include "check.hpp"

#include "base/crypto.hpp"
#include "base/time.hpp"
#include "base/url.hpp"

using namespace di;

TEST_CASE("sha256 / hmac known answers") {
    CHECK_EQ(crypto::sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(crypto::hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog"),
             std::string("f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"));
}

TEST_CASE("base64 round trip and url variant") {
    CHECK_EQ(crypto::base64_encode("hello"), std::string("aGVsbG8="));
    CHECK_EQ(*crypto::base64_decode("aGVsbG8"), std::string("hello"));
    CHECK_EQ(crypto::base64url_encode("\xfb\xff"), std::string("-_8"));
    CHECK_EQ(*crypto::base64_decode("-_8"), std::string("\xfb\xff"));
    CHECK(!crypto::base64_decode("a$b").has_value());
}

TEST_CASE("uuid v4 shape and constant time compare") {
    auto u = crypto::uuid_v4();
    CHECK_EQ(u.size(), std::size_t(36));
    CHECK_EQ(u[14], '4');
    CHECK(crypto::constant_time_equal("abc", "abc"));
    CHECK(!crypto::constant_time_equal("abc", "abd"));
    CHECK(!crypto::constant_time_equal("abc", "abcd"));
}

TEST_CASE("ed25519 verify (RFC 8032 test 1)") {
    auto pk = *crypto::hex_decode("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    auto sig = *crypto::hex_decode(
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    CHECK(crypto::ed25519_verify(pk, "", sig));
    CHECK(!crypto::ed25519_verify(pk, "x", sig));
    CHECK(!crypto::ed25519_verify("short", "", sig));
}

TEST_CASE("iso time formatting and parsing") {
    CHECK_EQ(time::iso_utc(0), std::string("1970-01-01T00:00:00.000Z"));
    CHECK_EQ(time::iso_utc(1761782096789), std::string("2025-10-29T23:54:56.789Z"));
    CHECK_EQ(*time::parse_iso("2025-10-29T23:54:56.789Z"), 1761782096789);
    CHECK_EQ(*time::parse_iso("2025-10-30T08:54:56.789+09:00"), 1761782096789);
    CHECK_EQ(*time::parse_iso("1970-01-02"), 86400000);
    CHECK_EQ(*time::parse_iso("1970-01-01 00:00:01"), 1000);
    CHECK(!time::parse_iso("nope").has_value());
    CHECK_EQ(time::sqlite_datetime_utc(1000), std::string("1970-01-01 00:00:01"));
}

TEST_CASE("url parse rejects userinfo and injection") {
    auto u = url::parse("https://Example.com:8443/a/b?x=1#f");
    CHECK(u.has_value());
    CHECK_EQ(u->host, std::string("example.com"));
    CHECK_EQ(u->port, 8443);
    CHECK_EQ(u->target, std::string("/a/b?x=1"));
    CHECK_EQ(u->origin(), std::string("https://example.com:8443"));
    CHECK(!url::parse("https://user@host/").has_value());
    CHECK(!url::parse("ftp://host/").has_value());
    CHECK(!url::parse("http://host/a b").has_value());
    CHECK(!url::parse("http://host/a\r\nX: y").has_value());
    CHECK_EQ(url::parse("http://[::1]:80/")->host, std::string("::1"));
    CHECK_EQ(url::encode_component("a b&日"), std::string("a%20b%26%E6%97%A5"));
    CHECK_EQ(*url::decode_component("a%20b"), std::string("a b"));
    CHECK(!url::decode_component("%zz").has_value());
    auto q = url::parse_query("a=1&b=x+y&a=2");
    CHECK_EQ(q.size(), std::size_t(3));
    CHECK_EQ(q[1].second, std::string("x y"));
}

DI_TEST_MAIN()
