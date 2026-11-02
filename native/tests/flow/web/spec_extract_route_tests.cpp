// w2-H: src/flow/web/spec-extract-route.ts の移植の確認 (multipart の読み取りの境界・各ルートの応答)。
// 抽出 (サイドカー)・gh・GitHub API は差し替える。
#include "../w2h_support.hpp"

#include <chrono>

#include "flow/web/spec_extract_route.hpp"

using namespace di;
using namespace di::flow;
using namespace di::flow::web;

namespace {

const std::string kBoundary = "----DiTestBoundary";

std::string part(const std::string& name, const std::optional<std::string>& filename, const std::string& data) {
    std::string s = "--" + kBoundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"";
    if (filename) s += "; filename=\"" + *filename + "\"";
    s += "\r\nContent-Type: application/octet-stream\r\n\r\n" + data + "\r\n";
    return s;
}

std::string form(const std::vector<std::string>& parts) {
    std::string s;
    for (const auto& p : parts) s += p;
    return s + "--" + kBoundary + "--\r\n";
}

http::Response post(const http::Router& r, const std::string& path, const std::string& body, const std::string& content_type) {
    http::Request req;
    req.method = "POST";
    req.path = path;
    req.target = path;
    req.body = body;
    req.remote_address = "127.0.0.1";
    req.headers.set("content-type", content_type);
    return r.handle(req);
}

Json body_of(const http::Response& r) { return parse_json_or_throw(r.body); }

SpecExtractDeps fake_deps(std::vector<std::string>* names = nullptr) {
    SpecExtractDeps d;
    d.extractor = [names](const std::string& bytes, const std::string& file_name, const std::string& label) {
        if (names) names->push_back(file_name + "|" + label);
        if (bytes == "BAD") throw std::runtime_error(label + ": 抽出できません");
        return std::string("抽出:") + std::to_string(bytes.size());
    };
    d.authStatus = [] { return GitHubCliAuthStatus{true, true, "Logged in"}; };
    d.githubToken = [] { return std::optional<std::string>("gho_fake_token_for_tests_only"); };
    return d;
}

} // namespace

TEST_CASE("multipart: boundary parameter and part parsing") {
    CHECK_EQ(*multipart_boundary("multipart/form-data; boundary=abc"), std::string("abc"));
    CHECK_EQ(*multipart_boundary("Multipart/Form-Data; charset=utf-8; BOUNDARY=\"a b;c\""), std::string("a b;c"));
    CHECK(!multipart_boundary("multipart/form-data"));
    CHECK(!multipart_boundary("multipart/form-data; boundary="));
    CHECK(!multipart_boundary("text/plain; boundary=abc"));
    CHECK(multipart_boundary("multipart/form-data; boundary=" + std::string(70, 'b')).has_value());
    CHECK(!multipart_boundary("multipart/form-data; boundary=" + std::string(71, 'b')));

    const auto parts = parse_multipart("preamble\r\n" + form({part("a", std::nullopt, "x"), part("file", "s.md", "line1\r\nline2")}), kBoundary);
    CHECK(parts.has_value());
    CHECK_EQ(parts->size(), 2u);
    CHECK(!(*parts)[0].filename);
    CHECK_EQ(*(*parts)[1].filename, std::string("s.md"));
    CHECK_EQ((*parts)[1].data, std::string("line1\r\nline2"));
    CHECK(parse_multipart("--" + kBoundary + "--", kBoundary)->empty());
}

TEST_CASE("multipart: broken and oversized inputs are refused") {
    CHECK(!parse_multipart("", kBoundary));
    CHECK(!parse_multipart("no boundary here", kBoundary));
    CHECK(!parse_multipart(part("file", "a.md", "x"), kBoundary));  // 閉じていない
    CHECK(!parse_multipart("--" + kBoundary + "\r\nX-No-Disposition: 1\r\n\r\nx\r\n--" + kBoundary + "--", kBoundary));
    CHECK(!parse_multipart("--" + kBoundary + "\r\nContent-Disposition: attachment; name=\"f\"\r\n\r\nx\r\n--" + kBoundary + "--", kBoundary));
    CHECK(!parse_multipart("--" + kBoundary + "\r\nContent-Disposition: form-data\r\n\r\nx\r\n--" + kBoundary + "--", kBoundary));
    CHECK(!parse_multipart("--" + kBoundary + "\r\nbad header line\r\n\r\nx\r\n--" + kBoundary + "--", kBoundary));
    CHECK(!parse_multipart("--" + kBoundary + "garbage", kBoundary));
    // 見出しの上限ちょうど / +1
    MultipartLimits lim;
    const std::string head = "Content-Disposition: form-data; name=\"f\"";
    const std::string pad_exact = head + "\r\nX-Pad: " + std::string(lim.max_header_bytes - head.size() - 2 - 7, 'p');
    CHECK_EQ(pad_exact.size(), lim.max_header_bytes);
    CHECK(parse_multipart("--" + kBoundary + "\r\n" + pad_exact + "\r\n\r\nx\r\n--" + kBoundary + "--", kBoundary).has_value());
    CHECK(!parse_multipart("--" + kBoundary + "\r\n" + pad_exact + "p\r\n\r\nx\r\n--" + kBoundary + "--", kBoundary));
    // 部品の数の上限ちょうど / +1
    std::vector<std::string> ps(lim.max_parts, part("a", std::nullopt, "x"));
    CHECK(parse_multipart(form(ps), kBoundary).has_value());
    ps.push_back(part("a", std::nullopt, "x"));
    CHECK(!parse_multipart(form(ps), kBoundary));
    // 巨大な入力も線形で読む
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!parse_multipart(std::string(8u << 20, '-'), kBoundary));
    CHECK(!parse_multipart("--" + kBoundary + "\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\n" + std::string(8u << 20, '\r'), kBoundary));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
}

TEST_CASE("POST /api/spec/extract: same statuses and messages as TS") {
    std::vector<std::string> names;
    const auto router = spec_extract_routes(fake_deps(&names));
    const std::string ct = "multipart/form-data; boundary=" + kBoundary;

    auto r = post(router, "/api/spec/extract", "x", "application/json");
    CHECK_EQ(r.status, 400);
    CHECK_EQ(json_string(body_of(r), "error"), std::string("multipart/form-data が必要です"));
    CHECK_EQ(post(router, "/api/spec/extract", "garbage", ct).status, 400);
    r = post(router, "/api/spec/extract", "file=x", "application/x-www-form-urlencoded");
    CHECK_EQ(json_string(body_of(r), "error"), std::string("file フィールドが必要です"));
    r = post(router, "/api/spec/extract", form({part("file", std::nullopt, "text only")}), ct);
    CHECK_EQ(r.status, 400);
    CHECK_EQ(json_string(body_of(r), "error"), std::string("file フィールドが必要です"));

    r = post(router, "/api/spec/extract", form({part("file", "spec.md", "# 仕様\n本文")}), ct);
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, std::string(R"({"ok":true,"text":"# 仕様\n本文"})"));
    r = post(router, "/api/spec/extract", form({part("other", "x.md", "y"), part("file", "spec.pdf", "%PDF-1.4 xx")}), ct);
    CHECK_EQ(r.status, 200);
    CHECK_EQ(json_string(body_of(r), "text"), std::string("抽出:11"));
    CHECK_EQ(names.back(), std::string("spec.pdf|ファイル spec.pdf"));
    r = post(router, "/api/spec/extract", form({part("file", "spec.exe", "MZ")}), ct);
    CHECK_EQ(r.status, 415);
    CHECK_EQ(json_string(body_of(r), "error"), std::string("unsupported file format: spec.exe"));
    r = post(router, "/api/spec/extract", form({part("file", "", "x")}), ct);
    CHECK_EQ(json_string(body_of(r), "error"), std::string("unsupported file format: upload"));
    r = post(router, "/api/spec/extract", form({part("file", "spec.docx", "BAD")}), ct);
    CHECK_EQ(r.status, 422);

    // ファイルの上限ちょうど / +1 (413 の文言は TS と同じ)
    r = post(router, "/api/spec/extract", form({part("file", "big.txt", std::string(DEFAULT_MAX_SPEC_BYTES, 'a'))}), ct);
    CHECK_EQ(r.status, 200);
    r = post(router, "/api/spec/extract", form({part("file", "big.txt", std::string(DEFAULT_MAX_SPEC_BYTES + 1, 'a'))}), ct);
    CHECK_EQ(r.status, 413);
    CHECK_EQ(json_string(body_of(r), "error"),
             "ファイルが大きすぎます (" + std::to_string(DEFAULT_MAX_SPEC_BYTES + 1) + " > " + std::to_string(DEFAULT_MAX_SPEC_BYTES) + " bytes)");
    CHECK_EQ(router.max_body_for("POST", "/api/spec/extract"), kSpecExtractMaxBody);
    CHECK_EQ(router.max_body_for("POST", "/api/spec/github/check"), kSpecGithubCheckMaxBody);
}

TEST_CASE("GitHub auth / check routes") {
    auto deps = fake_deps();
    std::vector<std::string> urls;
    deps.fetchImpl = [&](const http::ClientRequest& req) {
        urls.push_back(req.url);
        CHECK_EQ(*req.headers.get("Authorization"), std::string("Bearer gho_fake_token_for_tests_only"));
        http::ClientResponse r;
        r.transport_ok = true;
        r.status = 200;
        if (req.url.find("/contents/") != std::string::npos) {
            if (req.url.find("missing") != std::string::npos) r.status = 404;
            r.body = json_stringify(Json{{"type", "file"}, {"size", 5}, {"sha", "abc"}, {"encoding", "base64"}, {"content", "aGVsbG8="},
                                         {"html_url", "https://github.com/o/r/blob/main/spec.md"}});
        } else {
            r.body = json_stringify(Json{{"private", true}, {"default_branch", "main"}, {"html_url", "https://github.com/o/r"}});
        }
        return r;
    };
    const auto router = spec_extract_routes(deps);
    http::Request get;
    get.method = "GET";
    get.path = get.target = "/api/spec/github/auth";
    CHECK_EQ(router.handle(get).body, std::string(R"({"ok":true,"auth":{"available":true,"authenticated":true,"output":"Logged in"}})"));

    auto r = post(router, "/api/spec/github/check", "{not json", "application/json");
    CHECK_EQ(r.status, 400);
    CHECK_EQ(json_string(body_of(r), "error"), std::string("repoUrl is required"));
    r = post(router, "/api/spec/github/check", R"({"repoUrl":"https://example.com/x"})", "application/json");
    CHECK_EQ(json_string(body_of(r), "error"), std::string("GitHub repository URL could not be parsed"));
    r = post(router, "/api/spec/github/check", R"({"repoUrl":" https://github.com/o/r ","path":"  "})", "application/json");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(json_stringify(body_of(r)["repo"]), std::string(R"({"owner":"o","repo":"r","private":true,"defaultBranch":"main","htmlUrl":"https://github.com/o/r"})"));
    r = post(router, "/api/spec/github/check", R"({"repoUrl":"https://github.com/o/r","path":"spec.md","ref":"main"})", "application/json");
    CHECK_EQ(r.status, 200);
    const Json b = body_of(r);
    CHECK_EQ(json_string(b, "text"), std::string("hello"));
    CHECK_EQ(json_string(b, "preview"), std::string("hello"));
    CHECK_EQ(json_number(b, "size"), 5.0);
    CHECK_EQ(json_string(b, "sha"), std::string("abc"));
    r = post(router, "/api/spec/github/check", R"({"repoUrl":"https://github.com/o/r","path":"missing.md"})", "application/json");
    CHECK_EQ(r.status, 422);
    CHECK(!json_bool(body_of(r), "readable", true));
    CHECK(json_string(body_of(r), "error").find("HTTP 404") != std::string::npos);
}

DI_TEST_MAIN()
