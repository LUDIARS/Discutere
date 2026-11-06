#pragma once
// S3 への単一ファイル PutObject (アーカイブ push)。移植元 src/backup/s3.ts。
//
// - StorageClass を指定して Glacier 系へ安価にアーカイブできる。
// - endpoint 指定で MinIO 等の S3 互換ストレージにも流せる (path-style、TS の forcePathStyle)。
// - AWS SDK は使わず、署名 (SigV4) は OpenSSL の SHA-256 / HMAC (base/crypto) で作り、di::http::fetch で送る。
// - 大きなファイル (multipart_threshold 超) はメモリに全部載せないよう multipart upload に切り替える
//   (AWS SDK の PutObject は stream で送っていた。C++ の fetch は本文を文字列で持つため)。
// - 認証情報: 引数の accessKeyId + secretAccessKey、無ければ env (AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY /
//   AWS_SESSION_TOKEN)、無ければ共有認証ファイル (~/.aws/credentials の AWS_PROFILE、既定 default)。
//   IAM ロール (IMDS / ECS) は未対応 (port-notes の「人間の判断が要ること」)。
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/json.hpp"

namespace di::backup {

struct S3UploadArgs {
    std::string bucket;
    std::string key;
    std::string filePath;
    std::string region;
    std::optional<std::string> storageClass;
    std::optional<std::string> endpoint;
    std::optional<std::string> accessKeyId;
    std::optional<std::string> secretAccessKey;
};

struct S3UploadResult {
    std::string bucket;
    std::string key;
    std::int64_t bytes = 0;
    std::string storageClass;
};

Json to_json(const S3UploadResult& r);

/** 期限・同時実行・大きさの上限。 */
struct S3Options {
    /** 1 要求 (PUT / part) の期限。 */
    std::chrono::milliseconds request_timeout{10 * 60 * 1000};
    std::chrono::milliseconds connect_timeout{15'000};
    /** これを超えるファイルは multipart upload にする。 */
    std::size_t multipart_threshold = 64u << 20;
    /** multipart の 1 part の大きさ (S3 の下限は 5 MiB。テストでは小さくできる)。 */
    std::size_t part_size = 64u << 20;
    /** S3 の応答本文の上限 (XML の応答は小さい)。 */
    std::size_t max_response_bytes = 1u << 20;
    /** 同時に走らせる upload の数 (プロセス全体で 1)。空きを待つ上限。 */
    std::chrono::milliseconds queue_wait{10 * 60 * 1000};
    /** テストの偽サーバ (loopback) 用。endpoint 指定時は運用者が設定した宛先なので true になる。 */
    bool allow_private_network = false;
    /** テスト用に時刻を固定する (0 = 現在時刻)。 */
    std::int64_t now_ms = 0;
};

/** 失敗は std::runtime_error (秘密値は文言に含めない)。 */
S3UploadResult upload_file_to_s3(const S3UploadArgs& args, const S3Options& options = {});

// ─── SigV4 (テスト用に公開) ──────────────────────────────────────────

struct Credentials {
    std::string accessKeyId;
    std::string secretAccessKey;
    std::optional<std::string> sessionToken;
};

/** 認証情報を決める (上の優先順)。見つからなければ nullopt。 */
std::optional<Credentials> resolve_credentials(const S3UploadArgs& args);

/** AWS の URI エンコード (unreserved 以外を %XX。encode_slash=false なら "/" は残す)。 */
std::string aws_uri_encode(std::string_view s, bool encode_slash);

/** kSigning = HMAC(HMAC(HMAC(HMAC("AWS4"+secret, date), region), service), "aws4_request") */
std::string sigv4_signing_key(const std::string& secret, const std::string& date, const std::string& region,
                              const std::string& service);

struct SigV4Input {
    std::string method;
    /** エンコード済みのパス (例 "/bucket/a%20b")。 */
    std::string canonical_uri;
    /** (名前, 値) のクエリ (エンコード前)。 */
    std::vector<std::pair<std::string, std::string>> query;
    /** 署名するヘッダ (名前は任意の大小、値は前後の空白を詰める)。host を含めること。 */
    std::vector<std::pair<std::string, std::string>> headers;
    std::string payload_hash;
    std::string amz_date;  // "YYYYMMDDTHHMMSSZ"
    std::string region;
    std::string service;
};

std::string sigv4_canonical_request(const SigV4Input& in);
std::string sigv4_string_to_sign(const SigV4Input& in);
/** Authorization ヘッダの値。 */
std::string sigv4_authorization(const SigV4Input& in, const std::string& access_key_id, const std::string& secret);

} // namespace di::backup
