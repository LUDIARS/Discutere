// src/flow/spec-source.ts の移植の確認 (tests/flow/spec-source.test.ts と src/flow/spec-source.test.ts の観点 + golden)。
// PDF/DOCX/PPTX/XLSX/DOC はサイドカー。抽出関数を差し替えた経路の確認と、実際のサイドカー (node) を使う確認を両方する。
#include "check.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "flow/spec_source.hpp"

using namespace di;
using namespace di::flow;

namespace {

Json fixture() {
    auto text = fs::read_file(test::fixtures_dir() + "/flow/w1e_spec_source.json");
    if (!text) throw test::Failure("fixture missing");
    return parse_json_or_throw(*text);
}

std::string b64(const Json& j) { return *crypto::base64_decode(j.get<std::string>()); }


http::ClientResponse response(std::string body, std::string content_type = "", int status = 200) {
    http::ClientResponse r;
    r.transport_ok = true;
    r.status = status;
    r.body = std::move(body);
    if (!content_type.empty()) r.headers.set("content-type", content_type);
    return r;
}

struct Recorder {
    std::vector<std::string> names, labels;
    SpecSidecarExtractor fn() {
        return [this](const std::string&, const std::string& name, const std::string& label) {
            names.push_back(name);
            labels.push_back(label);
            return std::string("抽出:") + name;
        };
    }
};

std::string message_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

struct TempDir {
    std::string path;
    TempDir() {
        path = fs::from_path(std::filesystem::temp_directory_path() / ("w1e-spec-" + crypto::uuid_v4()));
        fs::ensure_dir(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(fs::to_path(path), ec);
    }
};

// 無圧縮 (stored) の ZIP を作る (移植前の読み手は CRC を検査しない)。
std::string stored_zip(const std::vector<std::pair<std::string, std::string>>& entries) {
    auto u16 = [](std::string& s, unsigned v) { s.push_back(static_cast<char>(v & 0xff)); s.push_back(static_cast<char>((v >> 8) & 0xff)); };
    auto u32 = [&](std::string& s, std::uint32_t v) { u16(s, v & 0xffff); u16(s, v >> 16); };
    std::string locals, central;
    for (const auto& [name, data] : entries) {
        const auto offset = static_cast<std::uint32_t>(locals.size());
        std::string l;
        u32(l, 0x04034b50); u16(l, 20); u16(l, 0); u16(l, 0); u16(l, 0); u16(l, 0); u32(l, 0);
        u32(l, static_cast<std::uint32_t>(data.size())); u32(l, static_cast<std::uint32_t>(data.size()));
        u16(l, static_cast<unsigned>(name.size())); u16(l, 0);
        locals += l + name + data;
        std::string c;
        u32(c, 0x02014b50); u16(c, 20); u16(c, 20); u16(c, 0); u16(c, 0); u16(c, 0); u16(c, 0); u32(c, 0);
        u32(c, static_cast<std::uint32_t>(data.size())); u32(c, static_cast<std::uint32_t>(data.size()));
        u16(c, static_cast<unsigned>(name.size())); u16(c, 0); u16(c, 0); u16(c, 0); u16(c, 0); u32(c, 0); u32(c, offset);
        central += c + name;
    }
    std::string e;
    u32(e, 0x06054b50); u16(e, 0); u16(e, 0);
    u16(e, static_cast<unsigned>(entries.size())); u16(e, static_cast<unsigned>(entries.size()));
    u32(e, static_cast<std::uint32_t>(central.size())); u32(e, static_cast<std::uint32_t>(locals.size())); u16(e, 0);
    return locals + central + e;
}

bool node_available() {
    const auto root = env::get("DI_REPO_ROOT");
    return root && fs::is_dir(*root + "/node_modules/tsx");
}

} // namespace

TEST_CASE("GitHub URL parsing, textual detection and http detection match Node") {
    const Json fx = fixture();
    for (const auto& c : fx["github"]) {
        const auto r = parse_github_spec_url(c["u"].get<std::string>());
        if (c["r"].is_null()) {
            if (r) throw test::Failure("expected null for " + c["u"].get<std::string>());
            continue;
        }
        if (!r) throw test::Failure("expected parse for " + c["u"].get<std::string>());
        CHECK_EQ(r->owner, c["r"]["owner"].get<std::string>());
        CHECK_EQ(r->repo, c["r"]["repo"].get<std::string>());
        CHECK_EQ(r->path ? Json(*r->path) : Json(nullptr), c["r"].value("path", Json(nullptr)));
        CHECK_EQ(r->ref ? Json(*r->ref) : Json(nullptr), c["r"].value("ref", Json(nullptr)));
    }
    const auto b0 = build_github_spec_source("https://github.com/o/r", std::string(" docs/x.md "), std::string(" v2 "));
    CHECK_EQ(*b0->path, fx["build"][0]["path"].get<std::string>());
    CHECK_EQ(*b0->ref, fx["build"][0]["ref"].get<std::string>());
    const auto b1 = build_github_spec_source("https://github.com/o/r/blob/m/a.md", std::string("  "));
    CHECK_EQ(*b1->path, fx["build"][1]["path"].get<std::string>());
    CHECK(!build_github_spec_source("nope").has_value());
    for (const auto& c : fx["textual"]) CHECK_EQ(is_textual(b64(c["b"])), c["r"].get<bool>());
    for (const auto& c : fx["isHttp"]) CHECK_EQ(is_http_url(c["s"].get<std::string>()), c["r"].get<bool>());
}

TEST_CASE("text / HTML extraction and error messages match Node") {
    const Json fx = fixture();
    for (const auto& c : fx["extract"])
        CHECK_EQ(extract_spec_text_from_bytes(b64(c["bytes"]), c["name"].get<std::string>()), c["result"].get<std::string>());
    for (const auto& c : fx["errors"]) {
        const std::string bytes = b64(c["bytes"]);
        const std::string name = c["name"].get<std::string>();
        CHECK_EQ(message_of([&] { extract_spec_text_from_bytes(bytes, name); }), c["error"].get<std::string>());
    }
    for (const auto& c : fx["fetches"]) {
        ResolveSpecOpts o;
        const std::string body = c["body"].get<std::string>(), ct = c["ct"].get<std::string>();
        const int status = c["status"].get<int>();
        o.fetchImpl = [&](const http::ClientRequest& req) {
            CHECK(!req.allow_private_network);  // 利用者が指定する URL は SSRF 対策つき
            return response(body, ct, status);
        };
        const std::string u = c["u"].get<std::string>();
        if (c.contains("result")) CHECK_EQ(fetch_spec_text(u, o), c["result"].get<std::string>());
        else CHECK_EQ(message_of([&] { fetch_spec_text(u, o); }), c["error"].get<std::string>());
    }
}

TEST_CASE("binary formats route to the sidecar with the same labels (spec-source.test.ts)") {
    Recorder rec;
    ResolveSpecOpts o;
    o.extractor = rec.fn();
    o.fetchImpl = [](const http::ClientRequest& req) {
        if (str::ends_with(req.url, "doc.pdf")) return response("dummy", "application/pdf");
        if (str::ends_with(req.url, "slides")) return response("PK\x03\x04", "application/vnd.openxmlformats-officedocument.presentationml.presentation");
        return response("dummy");
    };
    CHECK_EQ(fetch_spec_text("https://x/doc.pdf", o), std::string("抽出:spec.pdf"));
    CHECK_EQ(fetch_spec_text("https://x/doc.docx", o), std::string("抽出:spec.docx"));
    CHECK_EQ(fetch_spec_text("https://x/slides", o), std::string("抽出:spec.pptx"));
    CHECK_EQ(fetch_spec_text("https://x/sheet.XSLX", o), std::string("抽出:spec.xlsx"));
    CHECK_EQ(fetch_spec_text("https://x/old.doc", o), std::string("抽出:spec.doc"));
    CHECK_EQ(rec.labels[0], std::string("URL https://x/doc.pdf"));
    CHECK_EQ(extract_spec_text_from_bytes("%PDF-1.4", "doc.md", o), std::string("抽出:spec.pdf"));
    CHECK_EQ(extract_spec_text_from_bytes("PK\x03\x04", "doc.docx", o), std::string("抽出:spec.docx"));
    CHECK_EQ(extract_spec_text_from_bytes("PK\x03\x04", "notes.md", o), std::string("抽出:spec.docx"));
    CHECK_EQ(extract_spec_text_from_bytes("x", "a.pptx", o), std::string("抽出:spec.pptx"));
    CHECK_EQ(rec.labels.back(), std::string("ファイル a.pptx"));
    CHECK(is_pdf_bytes("%PDF-"));
    CHECK(!is_pdf_bytes("%PD"));
    CHECK(is_zip_bytes("PK\x03\x04\x00"));
}

TEST_CASE("local files: gate, existence, size limit, routing") {
    TempDir dir;
    const std::string md = fs::join(dir.path, "spec.md");
    fs::write_file_atomic(md, "# ローカル仕様");
    Recorder rec;
    ResolveSpecOpts o;
    o.extractor = rec.fn();
    CHECK(str::contains(message_of([&] { resolve_spec_text(md, o); }), "許可されていません"));
    o.allowLocalPath = true;
    CHECK_EQ(resolve_spec_text(md, o), std::string("# ローカル仕様"));
    CHECK(str::contains(message_of([&] { read_spec_text_from_file(fs::join(dir.path, "missing.md"), o); }), "見つかりません"));
    CHECK(str::contains(message_of([&] { read_spec_text_from_file(fs::join(dir.path, "x.exe"), o); }), "unsupported spec file format"));
    // 上限ちょうどは読む、上限 + 1 は読まない
    o.maxBytes = 10;
    fs::write_file_atomic(md, std::string(10, 'a'));
    CHECK_EQ(read_spec_text_from_file(md, o), std::string(10, 'a'));
    fs::write_file_atomic(md, std::string(11, 'a'));
    CHECK(str::contains(message_of([&] { read_spec_text_from_file(md, o); }), "大きすぎます (11 > 10 bytes)"));
    o.maxBytes.reset();
    const std::string xlsx = fs::join(dir.path, "a.xlsx");
    fs::write_file_atomic(xlsx, "PK\x03\x04zz");
    CHECK_EQ(read_spec_text_from_file(xlsx, o), std::string("抽出:spec.xlsx"));
    fs::write_file_atomic(xlsx, "plain text in xlsx");
    CHECK_EQ(read_spec_text_from_file(xlsx, o), std::string("plain text in xlsx"));  // ZIP でない .xlsx はテキスト (移植前と同じ)
    CHECK_EQ(rec.labels.back(), "ファイル " + xlsx);
    CHECK(str::contains(message_of([&] { resolve_spec_text("  ", o); }), "仕様書ソースが空です"));
}

TEST_CASE("URL fetch bounds") {
    ResolveSpecOpts o;
    o.maxBytes = 5;
    std::size_t seen_limit = 0;
    o.fetchImpl = [&](const http::ClientRequest& req) {
        seen_limit = req.max_response_bytes;
        return response("01234");
    };
    CHECK_EQ(fetch_spec_text("https://x/a.txt", o), std::string("01234"));
    CHECK_EQ(seen_limit, std::size_t{5});
    o.fetchImpl = [](const http::ClientRequest&) {
        http::ClientResponse r;
        r.error = "response too large";
        return r;
    };
    CHECK(str::contains(message_of([&] { fetch_spec_text("https://x/a.txt", o); }), "大きすぎます"));
    o.fetchImpl = [](const http::ClientRequest&) { return response("0123456789"); };
    CHECK(str::contains(message_of([&] { fetch_spec_text("https://x/y", o); }), "大きすぎます (10 > 5 bytes)"));
    o.fetchImpl = [](const http::ClientRequest&) {
        http::ClientResponse r;
        r.error = "blocked: 10.0.0.1 resolves to a non-public address";
        return r;
    };
    CHECK(str::contains(message_of([&] { fetch_spec_text("http://internal/a.md", o); }), "blocked"));
}

TEST_CASE("huge HTML is stripped in linear time") {
    std::string html;
    for (int i = 0; i < 20'000; ++i) html += "<p class=x>段落 &amp; <script>s</script>";
    html += std::string(2u << 20, '<');  // 閉じない < の連続 (二乗時間を誘う入力)
    const auto start = std::chrono::steady_clock::now();
    const auto text = extract_spec_text_from_bytes(html, "big.html");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(str::starts_with(text, "段落 & 段落 &"));
    CHECK(ms < 5000);
    std::string scripts;
    for (int i = 0; i < 200'000; ++i) scripts += "<script>";
    const auto s2 = std::chrono::steady_clock::now();
    extract_spec_text_from_bytes(scripts, "s.htm");
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s2).count() < 5000);
}

TEST_CASE("GitHub contents API: base64 file, directory, too large, errors") {
    ResolveSpecOpts o;
    o.githubToken = "fake-token-for-test";
    std::string last_url, auth;
    o.fetchImpl = [&](const http::ClientRequest& req) {
        last_url = req.url;
        auth = req.headers.get("authorization").value_or("");
        Json body = Json{{"type", "file"}, {"size", 9}, {"encoding", "base64"}, {"content", "IyBz\ncGVj\nIGE="}, {"sha", "abc"}};
        if (str::contains(req.url, "/dir")) body = Json::array();
        if (str::contains(req.url, "big.md")) body["size"] = 6'000'000;
        if (str::contains(req.url, "missing")) return response("{}", "", 404);
        return response(json_stringify(body));
    };
    const auto r = fetch_github_spec_text({"o", "r", std::string("docs/a b.md"), std::string("feat/x")}, o);
    CHECK_EQ(r.text, std::string("# spec a"));
    CHECK_EQ(*r.sha, std::string("abc"));
    CHECK_EQ(last_url, std::string("https://api.github.com/repos/o/r/contents/docs/a%20b.md?ref=feat%2Fx"));
    CHECK_EQ(auth, std::string("Bearer fake-token-for-test"));
    CHECK(str::contains(message_of([&] { fetch_github_spec_text({"o", "r", std::string("dir.md"), std::nullopt}, o); }), "directory"));
    CHECK(str::contains(message_of([&] { fetch_github_spec_text({"o", "r", std::string("big.md"), std::nullopt}, o); }), "too large"));
    CHECK(str::contains(message_of([&] { fetch_github_spec_text({"o", "r", std::string("missing.md"), std::nullopt}, o); }), "HTTP 404"));
    CHECK(str::contains(message_of([&] { fetch_github_spec_text({"o", "r", std::nullopt, std::nullopt}, o); }), "requires a file path"));
    CHECK_EQ(resolve_spec_text("https://github.com/o/r/blob/main/x.md", o), std::string("# spec a"));
    o.fetchImpl = [](const http::ClientRequest&) {
        return response(R"({"private":true,"default_branch":"main","html_url":"https://github.com/o/r"})");
    };
    const auto info = fetch_github_repo_info({"o", "r", std::nullopt, std::nullopt}, o);
    CHECK(*info.isPrivate);
    CHECK_EQ(*info.defaultBranch, std::string("main"));
}

TEST_CASE("real sidecar: PPTX / XLSX through node, labels rewritten, input bounds") {
    if (!node_available()) {
        std::cout << "  (node / tsx not available; skip)\n";
        return;
    }
    const auto repo_root = env::get("DI_REPO_ROOT");
    env::set_for_test("DISCUTERE_REPO_ROOT", repo_root ? std::optional<std::string_view>(*repo_root) : std::nullopt);
    const std::string pptx = stored_zip({{"ppt/slides/slide2.xml", "<a:t>二枚目 &amp; 続き</a:t>"},
                                         {"ppt/slides/slide1.xml", "<a:t>一枚目</a:t>"},
                                         {"ppt/notesSlides/notesSlide1.xml", "<a:t>注記</a:t>"}});
    CHECK_EQ(extract_spec_text_from_bytes(pptx, "spec.pptx"), std::string("注記\n\n一枚目\n\n二枚目 & 続き"));
    const std::string xlsx = stored_zip({{"xl/worksheets/sheet1.xml", "<c><v>42</v></c>"}, {"xl/sharedStrings.xml", "<t>項目 &lt;A&gt;</t>"}});
    CHECK_EQ(extract_spec_text_from_bytes(xlsx, "spec.xlsx"), std::string("項目 <A>\n\n42"));
    // 壊れた ZIP は見出し付きの文言で失敗する (URL 経路は URL の見出しに戻る)
    ResolveSpecOpts o;
    o.fetchImpl = [](const http::ClientRequest&) { return response("PK\x03\x04broken"); };
    CHECK_EQ(message_of([&] { fetch_spec_text("https://example.invalid/bomb.pptx", o); }),
             std::string("URL https://example.invalid/bomb.pptx: ZIP central directory was not found"));
    CHECK_EQ(message_of([&] { extract_spec_text_from_bytes("PK\x03\x04broken", "z.xlsx"); }),
             std::string("ファイル z.xlsx: ZIP central directory was not found"));
    // 上限を超える入力はサイドカーを起動せずに拒否する
    CHECK(str::contains(message_of([&] { extract_spec_text_from_bytes(std::string(kMaxSpecSidecarInputBytes + 1, 'x'), "a.pdf"); }),
                        "大きすぎます"));
    env::set_for_test("DISCUTERE_REPO_ROOT", std::nullopt);
}

DI_TEST_MAIN()
