// s3 のテスト (1/2)。準備は s3_support.hpp。
#include "s3_support.hpp"

using namespace s3_support;

TEST_CASE("SigV4: 署名鍵の導出 (AWS 文書の例)") {
    CHECK_EQ(crypto::hex_encode(sigv4_signing_key(kExampleSecret, "20120215", "us-east-1", "iam")),
             std::string("f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d"));
}

TEST_CASE("SigV4: aws-sig-v4-test-suite get-vanilla") {
    SigV4Input in;
    in.method = "GET";
    in.canonical_uri = "/";
    in.headers = {{"Host", "example.amazonaws.com"}, {"X-Amz-Date", "20150830T123600Z"}};
    in.payload_hash = kEmptyHash;
    in.amz_date = "20150830T123600Z";
    in.region = "us-east-1";
    in.service = "service";
    CHECK_EQ(sigv4_canonical_request(in),
             std::string("GET\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\n") + kEmptyHash);
    CHECK_EQ(sigv4_string_to_sign(in), std::string("AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n"
                                                   "bb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63"));
    CHECK_EQ(sigv4_authorization(in, "AKIDEXAMPLE", kExampleSecret),
             std::string("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, "
                         "SignedHeaders=host;x-amz-date, Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31"));
}

TEST_CASE("SigV4: aws-sig-v4-test-suite get-vanilla-query-order-key-case") {
    SigV4Input in;
    in.method = "GET";
    in.canonical_uri = "/";
    in.query = {{"Param2", "value2"}, {"Param1", "value1"}};
    in.headers = {{"Host", "example.amazonaws.com"}, {"X-Amz-Date", "20150830T123600Z"}};
    in.payload_hash = kEmptyHash;
    in.amz_date = "20150830T123600Z";
    in.region = "us-east-1";
    in.service = "service";
    CHECK(str::contains(sigv4_canonical_request(in), "\nParam1=value1&Param2=value2\n"));
    CHECK(str::ends_with(sigv4_authorization(in, "AKIDEXAMPLE", kExampleSecret),
                         "Signature=b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500"));
}

TEST_CASE("SigV4: S3 GetObject の例 (examplebucket / Range)") {
    SigV4Input in;
    in.method = "GET";
    in.canonical_uri = "/test.txt";
    in.headers = {{"Host", "examplebucket.s3.amazonaws.com"},
                  {"Range", "bytes=0-9"},
                  {"x-amz-content-sha256", kEmptyHash},
                  {"x-amz-date", "20130524T000000Z"}};
    in.payload_hash = kEmptyHash;
    in.amz_date = "20130524T000000Z";
    in.region = "us-east-1";
    in.service = "s3";
    CHECK(str::ends_with(sigv4_authorization(in, "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"),
                         "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
                         "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"));
}

TEST_CASE("URI エンコード") {
    CHECK_EQ(aws_uri_encode("a b/日本.tar.gz~", false), std::string("a%20b/%E6%97%A5%E6%9C%AC.tar.gz~"));
    CHECK_EQ(aws_uri_encode("a/b+c=", true), std::string("a%2Fb%2Bc%3D"));
}

DI_TEST_MAIN()
