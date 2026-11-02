// src/backup/s3.ts の移植テスト。
// - SigV4: AWS 公開のテストベクタ (aws-sig-v4-test-suite get-vanilla / get-vanilla-query-order-key-case、
//   署名鍵導出の例 (IAM 20120215)、S3 GetObject の例 (examplebucket / Range))。
// - 偽の S3 (di::http::Server) へ実際に PUT し、受け取った要求から署名を計算し直して照合する。
// - multipart・失敗時の中断・期限・応答上限・同時実行の上限・認証情報の優先順。
// 認証情報はすべて AWS 文書の例示用の値か明らかな偽物。
#include <atomic>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "../check.hpp"
#include "backup/s3.hpp"
#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "base/url.hpp"
#include "infra/http_server.hpp"

using namespace di;
using namespace di::backup;

namespace {

const std::string kExampleSecret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
const std::string kEmptyHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

std::string make_temp_dir() {
    auto dir = std::filesystem::temp_directory_path() / ("di-s3-test-" + crypto::uuid_v4());
    std::filesystem::create_directories(dir);
    return fs::from_path(std::filesystem::canonical(dir));
}

/** 受け取った要求から署名を計算し直す (signed headers の一覧に従う)。 */
std::string expected_authorization(const http::Request& r, const std::string& secret) {
    const std::string auth = r.header("authorization").value_or("");
    const auto sh = auth.find("SignedHeaders=");
    const auto comma = auth.find(',', sh);
    const auto names = str::split(auth.substr(sh + 14, comma - sh - 14), ";");
    SigV4Input in;
    in.method = r.method;
    in.canonical_uri = r.path;
    for (const auto& [k, v] : url::parse_query(r.query_string)) in.query.emplace_back(k, v);
    for (const auto& n : names) in.headers.emplace_back(n, r.header(n).value_or(""));
    in.payload_hash = r.header("x-amz-content-sha256").value_or("");
    in.amz_date = r.header("x-amz-date").value_or("");
    in.region = "ap-northeast-1";
    in.service = "s3";
    const auto cred = auth.find("Credential=");
    const std::string key_id = auth.substr(cred + 11, auth.find('/', cred) - cred - 11);
    return sigv4_authorization(in, key_id, secret);
}

struct FakeS3 {
    std::mutex mutex;
    std::vector<std::string> log;  // "METHOD path?query"
    std::map<int, std::string> parts;
    std::string object;
    std::string storage_class;
    std::string content_type;
    bool signature_ok = true;
    int fail_status = 0;
    bool fail_part = false;
    std::string secret = "fake-secret-not-real";
    std::shared_ptr<http::Router> router = std::make_shared<http::Router>();
    std::unique_ptr<http::Server> server;

    FakeS3() {
        auto check = [this](http::Request& r) {
            std::lock_guard lock(mutex);
            log.push_back(r.method + " " + r.target);
            if (r.header("authorization").value_or("") != expected_authorization(r, secret)) signature_ok = false;
            if (r.header("x-amz-content-sha256").value_or("") != crypto::sha256_hex(r.body)) signature_ok = false;
        };
        router->put("/bkt/*", [this, check](http::Request& r) {
            check(r);
            if (fail_status) return http::Response::bytes("<Error><Code>AccessDenied</Code></Error>", "application/xml", fail_status);
            std::lock_guard lock(mutex);
            if (auto part = r.query("partNumber")) {
                if (fail_part && *part == "2") return http::Response::text("boom", 500);
                parts[std::stoi(*part)] = r.body;
                auto res = http::Response::text("");
                res.headers.set("ETag", "\"etag-" + *part + "\"");
                return res;
            }
            object = r.body;
            storage_class = r.header("x-amz-storage-class").value_or("");
            content_type = r.header("content-type").value_or("");
            return http::Response::text("");
        }, {16u << 20});
        router->post("/bkt/*", [this, check](http::Request& r) {
            check(r);
            std::lock_guard lock(mutex);
            if (r.query("uploads")) {
                storage_class = r.header("x-amz-storage-class").value_or("");
                return http::Response::bytes("<InitiateMultipartUploadResult><UploadId>up&amp;1</UploadId></InitiateMultipartUploadResult>",
                                             "application/xml");
            }
            if (r.query("uploadId").value_or("") != "up&1") return http::Response::text("bad upload id", 400);
            for (const auto& [n, data] : parts) {
                if (!str::contains(r.body, "<PartNumber>" + std::to_string(n) + "</PartNumber><ETag>&quot;etag-" + std::to_string(n)))
                    return http::Response::text("<Error><Code>InvalidPart</Code></Error>", 200);
                object += data;
            }
            return http::Response::bytes("<CompleteMultipartUploadResult/>", "application/xml");
        }, {1u << 20});
        router->del("/bkt/*", [this, check](http::Request& r) {
            check(r);
            return http::Response::empty(204);
        });
        http::ServerOptions o;
        o.port = 0;
        server = std::make_unique<http::Server>(o, router);
        server->start();
    }
    ~FakeS3() { server->stop(); }
    std::string endpoint() const { return "http://127.0.0.1:" + std::to_string(server->bound_port()); }
};

S3UploadArgs args_for(const FakeS3& s3, const std::string& file) {
    S3UploadArgs a;
    a.bucket = "bkt";
    a.key = "discutere/2026 11/学習データ.tar.gz";
    a.filePath = file;
    a.region = "ap-northeast-1";
    a.storageClass = "GLACIER";
    a.endpoint = s3.endpoint();
    a.accessKeyId = "AKIDFAKEFAKE";
    a.secretAccessKey = s3.secret;
    return a;
}

std::string write_file(const std::string& dir, const std::string& name, const std::string& data) {
    const std::string p = fs::join(dir, name);
    CHECK(fs::write_file_atomic(p, data));
    return p;
}

}  // namespace

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

TEST_CASE("偽の S3 へ PutObject (署名・ヘッダ・本文を検証)") {
    FakeS3 s3;
    const std::string dir = make_temp_dir();
    const std::string data = "\x1f\x8b" + std::string(5000, 'z');
    const auto file = write_file(dir, "a.tar.gz", data);
    const auto r = upload_file_to_s3(args_for(s3, file));
    CHECK_EQ(json_stringify(to_json(r)),
             std::string(R"({"bucket":"bkt","key":"discutere/2026 11/学習データ.tar.gz","bytes":5002,"storageClass":"GLACIER"})"));
    CHECK(s3.signature_ok);
    CHECK_EQ(s3.object, data);
    CHECK_EQ(s3.storage_class, std::string("GLACIER"));
    CHECK_EQ(s3.content_type, std::string("application/gzip"));
    CHECK_EQ(s3.log.size(), std::size_t{1});
    CHECK_EQ(s3.log[0], std::string("PUT /bkt/discutere/2026%2011/%E5%AD%A6%E7%BF%92%E3%83%87%E3%83%BC%E3%82%BF.tar.gz"));
    // storageClass 未指定は STANDARD
    auto a = args_for(s3, file);
    a.storageClass.reset();
    CHECK_EQ(upload_file_to_s3(a).storageClass, std::string("STANDARD"));
    std::filesystem::remove_all(fs::to_path(dir));
}

TEST_CASE("multipart: 閾値ちょうどは単一 PUT・+1 は multipart") {
    FakeS3 s3;
    const std::string dir = make_temp_dir();
    S3Options o;
    o.multipart_threshold = 1000;
    o.part_size = 400;
    const auto exact = write_file(dir, "exact", std::string(1000, 'e'));
    upload_file_to_s3(args_for(s3, exact), o);
    CHECK_EQ(s3.log.size(), std::size_t{1});
    s3.log.clear();
    s3.object.clear();
    std::string data;
    for (int i = 0; i < 1001; ++i) data.push_back(static_cast<char>('a' + i % 26));
    const auto big = write_file(dir, "big", data);
    const auto r = upload_file_to_s3(args_for(s3, big), o);
    CHECK_EQ(r.bytes, std::int64_t{1001});
    CHECK(s3.signature_ok);
    CHECK_EQ(s3.object, data);
    CHECK_EQ(s3.parts.size(), std::size_t{3});
    CHECK_EQ(s3.storage_class, std::string("GLACIER"));
    CHECK(str::starts_with(s3.log.front(), "POST /bkt/") && str::ends_with(s3.log.front(), "?uploads="));
    // part の失敗は中断 (DELETE) して例外
    s3.log.clear();
    s3.parts.clear();
    s3.fail_part = true;
    CHECK_THROWS(upload_file_to_s3(args_for(s3, big), o));
    CHECK(str::starts_with(s3.log.back(), "DELETE /bkt/"));
    std::filesystem::remove_all(fs::to_path(dir));
}

TEST_CASE("失敗: 403 は例外 (秘密値を含めない)・応答上限・期限・同時実行") {
    FakeS3 s3;
    const std::string dir = make_temp_dir();
    const auto file = write_file(dir, "a", "data");
    s3.fail_status = 403;
    try {
        upload_file_to_s3(args_for(s3, file));
        CHECK(false);
    } catch (const std::runtime_error& e) {
        const std::string m = e.what();
        CHECK(str::contains(m, "403"));
        CHECK(str::contains(m, "AccessDenied"));
        CHECK(!str::contains(m, s3.secret));
    }
    s3.fail_status = 0;
    // 応答上限: 偽サーバの応答本文より小さい上限なら失敗
    S3Options small;
    small.max_response_bytes = 0;
    s3.fail_status = 403;
    CHECK_THROWS(upload_file_to_s3(args_for(s3, file), small));
    s3.fail_status = 0;

    // 期限と同時実行: 応答の遅いサーバ
    auto router = std::make_shared<http::Router>();
    router->put("/bkt/*", [](http::Request&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        return http::Response::text("");
    });
    http::ServerOptions so;
    so.port = 0;
    http::Server slow(so, router);
    slow.start();
    auto a = args_for(s3, file);
    a.endpoint = "http://127.0.0.1:" + std::to_string(slow.bound_port());
    S3Options quick;
    quick.request_timeout = std::chrono::milliseconds(200);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_THROWS(upload_file_to_s3(a, quick));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(700));
    std::atomic<bool> first_done{false};
    std::thread first([&] {
        try {
            upload_file_to_s3(a);
        } catch (...) {
        }
        first_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    S3Options no_wait;
    no_wait.queue_wait = std::chrono::milliseconds(50);
    try {
        upload_file_to_s3(a, no_wait);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(str::contains(e.what(), "another upload"));
    }
    first.join();
    CHECK(first_done.load());
    slow.stop();
    std::filesystem::remove_all(fs::to_path(dir));
}

TEST_CASE("認証情報: 引数 > env > 共有認証ファイル (AWS_PROFILE)") {
    const std::string dir = make_temp_dir();
    const auto cred_file = write_file(dir, "credentials",
                                      "[default]\naws_access_key_id = FILEDEFAULT\naws_secret_access_key = file-secret\n"
                                      "[other]\naws_access_key_id=FILEOTHER\naws_secret_access_key=other-secret\naws_session_token=tok\n");
    env::set_for_test("AWS_ACCESS_KEY_ID", std::nullopt);
    env::set_for_test("AWS_SECRET_ACCESS_KEY", std::nullopt);
    env::set_for_test("AWS_SHARED_CREDENTIALS_FILE", cred_file);
    S3UploadArgs a;
    CHECK_EQ(resolve_credentials(a)->accessKeyId, std::string("FILEDEFAULT"));
    env::set_for_test("AWS_PROFILE", "other");
    CHECK_EQ(*resolve_credentials(a)->sessionToken, std::string("tok"));
    env::set_for_test("AWS_ACCESS_KEY_ID", "ENVKEY");
    env::set_for_test("AWS_SECRET_ACCESS_KEY", "env-secret");
    CHECK_EQ(resolve_credentials(a)->accessKeyId, std::string("ENVKEY"));
    a.accessKeyId = "ARGKEY";
    a.secretAccessKey = "arg-secret";
    const auto c = resolve_credentials(a);
    CHECK_EQ(c->accessKeyId, std::string("ARGKEY"));
    CHECK(!c->sessionToken.has_value());  // 明示の認証情報には session token を付けない (TS と同じ)
    env::set_for_test("AWS_ACCESS_KEY_ID", std::nullopt);
    env::set_for_test("AWS_SECRET_ACCESS_KEY", std::nullopt);
    env::set_for_test("AWS_PROFILE", std::nullopt);
    env::set_for_test("AWS_SHARED_CREDENTIALS_FILE", fs::join(dir, "missing"));
    CHECK(!resolve_credentials(S3UploadArgs{}).has_value());
    env::set_for_test("AWS_SHARED_CREDENTIALS_FILE", std::nullopt);
    std::filesystem::remove_all(fs::to_path(dir));
}

DI_TEST_MAIN()
