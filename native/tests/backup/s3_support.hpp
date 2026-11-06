#pragma once
// s3_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
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

namespace s3_support {

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

} // namespace s3_support
