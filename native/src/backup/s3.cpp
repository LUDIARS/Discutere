#include "backup/s3.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "base/url.hpp"
#include "infra/http_client.hpp"

namespace di::backup {

namespace {

constexpr const char* kAlgorithm = "AWS4-HMAC-SHA256";
/** 共有認証ファイルの上限。 */
constexpr std::size_t kMaxCredentialsFileBytes = 256 * 1024;
/** CompleteMultipartUpload の part 数の上限 (S3 の仕様)。 */
constexpr std::size_t kMaxParts = 10'000;

std::timed_mutex& upload_slot() {
    static std::timed_mutex m;
    return m;
}

std::string trim_header_value(std::string_view v) { return str::squish(v); }

/** "YYYYMMDDTHHMMSSZ" */
std::string amz_date_of(std::int64_t ms) {
    const auto c = time::to_civil_utc(ms);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02dZ", c.year, c.month, c.day, c.hour, c.minute, c.second);
    return buf;
}

/** ~/.aws/credentials (INI) の [profile] から読む。 */
std::optional<Credentials> credentials_from_file() {
    std::string path;
    if (auto p = env::get_nonempty("AWS_SHARED_CREDENTIALS_FILE")) path = *p;
    else if (auto h = env::get_nonempty("HOME")) path = fs::join(fs::join(*h, ".aws"), "credentials");
    else if (auto u = env::get_nonempty("USERPROFILE")) path = fs::join(fs::join(*u, ".aws"), "credentials");
    else return std::nullopt;
    if (!fs::is_file(path)) return std::nullopt;
    auto text = fs::read_file(path, kMaxCredentialsFileBytes);
    if (!text) return std::nullopt;
    const std::string profile = env::get_nonempty("AWS_PROFILE").value_or("default");
    bool in_profile = false;
    Credentials c;
    for (const auto& raw : str::split_lines(*text)) {
        const std::string line = str::trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            in_profile = str::trim(std::string_view(line).substr(1, line.size() - 2)) == profile;
            continue;
        }
        if (!in_profile) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = str::to_lower_ascii(str::trim(std::string_view(line).substr(0, eq)));
        const std::string v = str::trim(std::string_view(line).substr(eq + 1));
        if (k == "aws_access_key_id") c.accessKeyId = v;
        else if (k == "aws_secret_access_key") c.secretAccessKey = v;
        else if (k == "aws_session_token") c.sessionToken = v;
    }
    if (c.accessKeyId.empty() || c.secretAccessKey.empty()) return std::nullopt;
    return c;
}

/** bucket を virtual-hosted (bucket.s3.region.amazonaws.com) で使えるか (DNS 互換・ドット無し)。 */
bool dns_compatible(const std::string& bucket) {
    if (bucket.size() < 3 || bucket.size() > 63) return false;
    if (bucket.front() == '-' || bucket.back() == '-') return false;
    return std::all_of(bucket.begin(), bucket.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

struct Target {
    std::string base;  // "https://host[:port]"
    std::string uri;   // エンコード済みパス
    bool allow_private = false;
};

Target object_target(const S3UploadArgs& args, bool allow_private) {
    const std::string key_path = aws_uri_encode(args.key, false);
    Target t;
    if (args.endpoint) {
        // forcePathStyle: <endpoint>/<bucket>/<key>。endpoint は運用者の設定値 (MinIO 等のローカルも可)。
        auto u = url::parse(*args.endpoint);
        if (!u || (u->scheme != "http" && u->scheme != "https")) throw std::runtime_error("backup endpoint must be an http(s) URL");
        std::string prefix = u->path == "/" ? "" : u->path;
        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
        t.base = u->scheme + "://" + u->host_for_header() + (u->explicit_port ? ":" + std::to_string(u->port) : "");
        t.uri = prefix + "/" + aws_uri_encode(args.bucket, true) + "/" + key_path;
        t.allow_private = true;
    } else if (dns_compatible(args.bucket)) {
        t.base = "https://" + args.bucket + ".s3." + args.region + ".amazonaws.com";
        t.uri = "/" + key_path;
        t.allow_private = allow_private;
    } else {
        t.base = "https://s3." + args.region + ".amazonaws.com";
        t.uri = "/" + aws_uri_encode(args.bucket, true) + "/" + key_path;
        t.allow_private = allow_private;
    }
    return t;
}

std::string build_query(const std::vector<std::pair<std::string, std::string>>& q) {
    auto sorted = q;
    std::sort(sorted.begin(), sorted.end());
    std::string out;
    for (const auto& [k, v] : sorted) {
        if (!out.empty()) out += '&';
        out += aws_uri_encode(k, true) + "=" + aws_uri_encode(v, true);
    }
    return out;
}

/** XML の <tag>…</tag> の最初の中身 (S3 の小さな応答だけに使う)。 */
std::optional<std::string> xml_tag(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const auto a = xml.find(open);
    if (a == std::string::npos) return std::nullopt;
    const auto b = xml.find(close, a + open.size());
    if (b == std::string::npos) return std::nullopt;
    return str::decode_entities(xml.substr(a + open.size(), b - a - open.size()));
}

std::string s3_error(const std::string& what, const http::ClientResponse& r) {
    if (!r.transport_ok) return what + " failed: " + r.error;
    std::string code = xml_tag(r.body, "Code").value_or("");
    return what + " failed: status " + std::to_string(r.status) + (code.empty() ? "" : " (" + code + ")");
}

std::optional<std::string> read_range(const std::string& path, std::uint64_t offset, std::size_t len) {
    std::ifstream in(fs::to_path(path), std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(static_cast<std::streamoff>(offset));
    std::string buf(len, '\0');
    in.read(buf.data(), static_cast<std::streamsize>(len));
    if (static_cast<std::size_t>(in.gcount()) != len) return std::nullopt;
    return buf;
}

class Signer {
public:
    Signer(const S3UploadArgs& args, const S3Options& options, Credentials creds)
        : args_(args), options_(options), creds_(std::move(creds)), target_(object_target(args, options.allow_private_network)) {}

    http::ClientResponse send(const std::string& method, const std::vector<std::pair<std::string, std::string>>& query,
                              std::vector<std::pair<std::string, std::string>> extra_headers, const std::string& body) {
        const auto host = url::parse(target_.base + "/");
        if (!host) throw std::runtime_error("invalid S3 URL");
        const std::string host_header = host->host_for_header() + (host->explicit_port ? ":" + std::to_string(host->port) : "");
        SigV4Input in;
        in.method = method;
        in.canonical_uri = target_.uri;
        in.query = query;
        in.payload_hash = crypto::sha256_hex(body);
        in.amz_date = amz_date_of(options_.now_ms ? options_.now_ms : time::now_ms());
        in.region = args_.region;
        in.service = "s3";
        in.headers = std::move(extra_headers);
        in.headers.emplace_back("host", host_header);
        in.headers.emplace_back("x-amz-content-sha256", in.payload_hash);
        in.headers.emplace_back("x-amz-date", in.amz_date);
        if (creds_.sessionToken) in.headers.emplace_back("x-amz-security-token", *creds_.sessionToken);

        http::ClientRequest req;
        req.method = method;
        const std::string q = build_query(query);
        req.url = target_.base + target_.uri + (q.empty() ? "" : "?" + q);
        for (const auto& [k, v] : in.headers)
            if (k != "host") req.headers.set(k, v);
        req.headers.set("Authorization", sigv4_authorization(in, creds_.accessKeyId, creds_.secretAccessKey));
        req.body = body;
        req.timeout = options_.request_timeout;
        req.connect_timeout = options_.connect_timeout;
        req.max_response_bytes = options_.max_response_bytes;
        req.allow_private_network = target_.allow_private;
        req.max_redirects = 0;
        return http::fetch(req);
    }

private:
    const S3UploadArgs& args_;
    const S3Options& options_;
    Credentials creds_;
    Target target_;
};

}  // namespace

Json to_json(const S3UploadResult& r) {
    Json o = Json::object();
    o["bucket"] = r.bucket;
    o["key"] = r.key;
    o["bytes"] = r.bytes;
    o["storageClass"] = r.storageClass;
    return o;
}

std::string aws_uri_encode(std::string_view s, bool encode_slash) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved || (c == '/' && !encode_slash)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string sigv4_signing_key(const std::string& secret, const std::string& date, const std::string& region,
                              const std::string& service) {
    const auto k_date = crypto::hmac_sha256("AWS4" + secret, date);
    const auto k_region = crypto::hmac_sha256(k_date, region);
    const auto k_service = crypto::hmac_sha256(k_region, service);
    return crypto::hmac_sha256(k_service, "aws4_request");
}

std::string sigv4_canonical_request(const SigV4Input& in) {
    std::vector<std::pair<std::string, std::string>> headers;
    for (const auto& [k, v] : in.headers) headers.emplace_back(str::to_lower_ascii(k), trim_header_value(v));
    std::stable_sort(headers.begin(), headers.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string canonical_headers;
    std::string signed_headers;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        // 同名ヘッダはカンマで連結する (SigV4 の規則)。
        if (i > 0 && headers[i].first == headers[i - 1].first) {
            canonical_headers.pop_back();
            canonical_headers += "," + headers[i].second + "\n";
            continue;
        }
        canonical_headers += headers[i].first + ":" + headers[i].second + "\n";
        if (!signed_headers.empty()) signed_headers += ";";
        signed_headers += headers[i].first;
    }
    return in.method + "\n" + in.canonical_uri + "\n" + build_query(in.query) + "\n" + canonical_headers + "\n" + signed_headers +
           "\n" + in.payload_hash;
}

std::string sigv4_string_to_sign(const SigV4Input& in) {
    const std::string date = in.amz_date.substr(0, 8);
    return std::string(kAlgorithm) + "\n" + in.amz_date + "\n" + date + "/" + in.region + "/" + in.service + "/aws4_request\n" +
           crypto::sha256_hex(sigv4_canonical_request(in));
}

std::string sigv4_authorization(const SigV4Input& in, const std::string& access_key_id, const std::string& secret) {
    const std::string date = in.amz_date.substr(0, 8);
    const std::string signature =
        crypto::hex_encode(crypto::hmac_sha256(sigv4_signing_key(secret, date, in.region, in.service), sigv4_string_to_sign(in)));
    std::vector<std::string> names;
    for (const auto& h : in.headers) names.push_back(str::to_lower_ascii(h.first));
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return std::string(kAlgorithm) + " Credential=" + access_key_id + "/" + date + "/" + in.region + "/" + in.service +
           "/aws4_request, SignedHeaders=" + str::join(names, ";") + ", Signature=" + signature;
}

std::optional<Credentials> resolve_credentials(const S3UploadArgs& args) {
    if (args.accessKeyId && !args.accessKeyId->empty() && args.secretAccessKey && !args.secretAccessKey->empty())
        return Credentials{*args.accessKeyId, *args.secretAccessKey, std::nullopt};
    auto id = env::get_nonempty("AWS_ACCESS_KEY_ID");
    auto secret = env::get_nonempty("AWS_SECRET_ACCESS_KEY");
    if (id && secret) return Credentials{*id, *secret, env::get_nonempty("AWS_SESSION_TOKEN")};
    return credentials_from_file();
}

S3UploadResult upload_file_to_s3(const S3UploadArgs& args, const S3Options& options) {
    if (args.bucket.empty() || args.key.empty()) throw std::runtime_error("S3 bucket and key are required");
    const auto size = fs::file_size(args.filePath);
    if (!size || !fs::is_file(args.filePath)) throw std::runtime_error("S3 upload: file not found");
    auto creds = resolve_credentials(args);
    if (!creds) throw std::runtime_error("S3 upload: credentials not found (args / env / shared credentials file)");
    const std::string storage_class = args.storageClass.value_or("STANDARD");

    std::unique_lock slot(upload_slot(), std::defer_lock);
    if (!slot.try_lock_for(options.queue_wait)) throw std::runtime_error("S3 upload: another upload is still running");

    Signer signer(args, options, *creds);
    const std::vector<std::pair<std::string, std::string>> object_headers = {
        {"content-type", "application/gzip"}, {"x-amz-storage-class", storage_class}};

    if (*size <= options.multipart_threshold) {
        auto body = fs::read_file(args.filePath, options.multipart_threshold);
        if (!body || body->size() != *size) throw std::runtime_error("S3 upload: file read failed");
        const auto r = signer.send("PUT", {}, object_headers, *body);
        if (!r.ok()) throw std::runtime_error(s3_error("PutObject", r));
    } else {
        const std::size_t part_size = std::max<std::size_t>(1, options.part_size);
        const std::uint64_t parts = (*size + part_size - 1) / part_size;
        if (parts > kMaxParts) throw std::runtime_error("S3 upload: too many parts");
        const auto created = signer.send("POST", {{"uploads", ""}}, object_headers, "");
        if (!created.ok()) throw std::runtime_error(s3_error("CreateMultipartUpload", created));
        const auto upload_id = xml_tag(created.body, "UploadId");
        if (!upload_id || upload_id->empty()) throw std::runtime_error("CreateMultipartUpload failed: no UploadId");
        try {
            std::string complete = "<CompleteMultipartUpload>";
            for (std::uint64_t i = 0; i < parts; ++i) {
                const std::uint64_t offset = i * part_size;
                const std::size_t len = static_cast<std::size_t>(std::min<std::uint64_t>(part_size, *size - offset));
                auto chunk = read_range(args.filePath, offset, len);
                if (!chunk) throw std::runtime_error("S3 upload: file read failed");
                const std::string number = std::to_string(i + 1);
                const auto r = signer.send("PUT", {{"partNumber", number}, {"uploadId", *upload_id}}, {}, *chunk);
                if (!r.ok()) throw std::runtime_error(s3_error("UploadPart", r));
                const auto etag = r.headers.get("ETag");
                if (!etag) throw std::runtime_error("UploadPart failed: no ETag");
                complete += "<Part><PartNumber>" + number + "</PartNumber><ETag>" + str::escape_html(*etag) + "</ETag></Part>";
            }
            complete += "</CompleteMultipartUpload>";
            const auto done = signer.send("POST", {{"uploadId", *upload_id}}, {{"content-type", "application/xml"}}, complete);
            // CompleteMultipartUpload は 200 で <Error> を返すことがある。
            if (!done.ok() || str::contains(done.body, "<Error>")) throw std::runtime_error(s3_error("CompleteMultipartUpload", done));
        } catch (...) {
            signer.send("DELETE", {{"uploadId", *upload_id}}, {}, "");  // 途中の part を残さない (失敗は無視)
            throw;
        }
    }
    return {args.bucket, args.key, static_cast<std::int64_t>(*size), storage_class};
}

} // namespace di::backup
