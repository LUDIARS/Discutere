// s3 のテスト (2/2)。準備は s3_support.hpp。
#include "s3_support.hpp"

using namespace s3_support;

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
