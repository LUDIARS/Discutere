#include "flow/spec_source.hpp"

#include <algorithm>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/fsutil.hpp"
#include "base/sidecar.hpp"
#include "base/strings.hpp"
#include "base/url.hpp"

namespace di::flow {

namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

void assert_supported_spec_file_name(std::string_view file_name, const std::string& label) {
    if (!is_supported_spec_file_name(file_name))
        fail(label + ": unsupported spec file format. Supported: " + str::join(supported_spec_extensions(), ", "));
}

bool is_pdf_ext(std::string_view p) { return spec_ext(p) == ".pdf"; }
bool is_docx_ext(std::string_view p) { return spec_ext(p) == ".docx"; }
bool is_doc_ext(std::string_view p) { return spec_ext(p) == ".doc"; }
bool is_pptx_ext(std::string_view p) { return spec_ext(p) == ".pptx"; }
bool is_xlsx_ext(std::string_view p) {
    const auto e = spec_ext(p);
    return e == ".xlsx" || e == ".xslx";
}
bool is_html_ext(std::string_view p) {
    const auto e = spec_ext(p);
    return e == ".html" || e == ".htm";
}

std::string decode_textual(std::string_view bytes, const std::string& label) {
    if (!is_textual(bytes))
        fail(label + " はテキストとして読めません (バイナリ形式の可能性。PDF は .pdf, DOCX は .docx を使用してください)");
    return decode_utf8_like_textdecoder(bytes);
}

std::string sidecar_extract(const ResolveSpecOpts& opts, const std::string& bytes, const char* kind_name, const std::string& label) {
    if (bytes.size() > kMaxSpecSidecarInputBytes)
        fail(label + ": 仕様書が大きすぎます (" + std::to_string(bytes.size()) + " > " + std::to_string(kMaxSpecSidecarInputBytes) + " bytes)");
    return opts.extractor ? opts.extractor(bytes, kind_name, label) : run_spec_extract_sidecar(bytes, kind_name, label);
}

bool ieq_suffix(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && str::iequals_ascii(s.substr(s.size() - suffix.size()), suffix);
}

std::string strip_git_suffix(std::string_view repo) {
    if (ieq_suffix(repo, ".git")) repo.remove_suffix(4);
    return std::string(repo);
}

bool has_js_space(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size())
        if (str::is_js_space(str::next_codepoint(s, i))) return true;
    return false;
}

// /^git@github\.com:([^/\s]+)\/([^/\s]+?)(?:\.git)?$/i
std::optional<GithubSpecSource> parse_github_ssh_url(std::string_view input) {
    const std::string t = str::trim(input);
    constexpr std::string_view prefix = "git@github.com:";
    if (t.size() < prefix.size() || !str::iequals_ascii(std::string_view(t).substr(0, prefix.size()), prefix)) return std::nullopt;
    const std::string_view rest = std::string_view(t).substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0) return std::nullopt;
    const std::string_view owner = rest.substr(0, slash);
    std::string_view repo = rest.substr(slash + 1);
    if (repo.empty() || repo.find('/') != std::string_view::npos || has_js_space(owner) || has_js_space(repo)) return std::nullopt;
    // 遅延一致の本体は 1 文字以上なので、".git" だけなら接尾辞として外さない
    if (repo.size() > 4 && ieq_suffix(repo, ".git")) repo.remove_suffix(4);
    return GithubSpecSource{std::string(owner), strip_git_suffix(repo), std::nullopt, std::nullopt};
}

std::vector<std::string> path_parts(std::string_view pathname) {
    std::vector<std::string> out;
    for (auto& p : str::split(pathname, "/"))
        if (!p.empty()) out.push_back(p);
    return out;
}

std::string join_from(const std::vector<std::string>& parts, std::size_t start) {
    std::vector<std::string> tail(parts.begin() + static_cast<std::ptrdiff_t>(std::min(start, parts.size())), parts.end());
    return str::join(tail, "/");
}

std::optional<std::string> query_get(const url::Url& u, std::string_view key) {
    for (const auto& [k, v] : url::parse_query(u.query))
        if (k == key) return v;
    return std::nullopt;
}

void set_github_headers(http::ClientRequest& req, const std::optional<std::string>& token) {
    req.headers.set("accept", "application/vnd.github+json");
    req.headers.set("user-agent", "LUDIARS-Discutere");
    if (token && !token->empty()) req.headers.set("authorization", "Bearer " + *token);
}

http::ClientResponse do_fetch(const ResolveSpecOpts& opts, const http::ClientRequest& req) {
    return opts.fetchImpl ? opts.fetchImpl(req) : http::fetch(req);
}

std::string encode_github_path(std::string_view path) {
    std::vector<std::string> parts;
    for (const auto& p : str::split(path, "/")) parts.push_back(url::encode_component(p));
    return str::join(parts, "/");
}

bool looks_like_size_limit(const std::string& error) {
    const std::string e = str::to_lower_ascii(error);
    return e.find("too large") != std::string::npos || e.find("limit") != std::string::npos;
}

} // namespace

const std::vector<std::string>& supported_spec_extensions() {
    static const std::vector<std::string> exts = {".doc",  ".docx", ".htm", ".html", ".json", ".md",   ".markdown", ".pdf",
                                                  ".pptx", ".text", ".txt", ".xlsx", ".xslx", ".yaml", ".yml"};
    return exts;
}

std::string spec_ext(std::string_view file_path) {
    // path.extname: 最後の区切りより後の basename で、先頭以外の最後の "." から
    const auto sep = file_path.find_last_of("/\\");
    std::string_view base = sep == std::string_view::npos ? file_path : file_path.substr(sep + 1);
    const auto dot = base.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return "";
    if (base.find_first_not_of('.') == std::string_view::npos) return "";
    return str::to_lower(base.substr(dot));
}

bool is_supported_spec_file_name(std::string_view file_name) {
    const auto e = spec_ext(file_name);
    const auto& exts = supported_spec_extensions();
    return std::find(exts.begin(), exts.end(), e) != exts.end();
}

bool is_pdf_bytes(std::string_view b) { return b.size() >= 4 && b[0] == 0x25 && b[1] == 0x50 && b[2] == 0x44 && b[3] == 0x46; }
bool is_zip_bytes(std::string_view b) { return b.size() >= 4 && b[0] == 0x50 && b[1] == 0x4b && b[2] == 0x03 && b[3] == 0x04; }

bool is_textual(std::string_view bytes) {
    if (bytes.empty()) return false;
    const std::size_t n = std::min<std::size_t>(bytes.size(), 4096);
    std::size_t suspicious = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto b = static_cast<unsigned char>(bytes[i]);
        if (b == 0) return false;  // NUL → バイナリ確定
        if (b < 0x09 || (b > 0x0d && b < 0x20)) ++suspicious;
    }
    return static_cast<double>(suspicious) / static_cast<double>(n) < 0.1;
}

std::string decode_utf8_like_textdecoder(std::string_view bytes) {
    // WHATWG Encoding の UTF-8 decoder (不正な部分列ごとに U+FFFD 1 つ)。先頭の BOM は落とす。
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.remove_prefix(3);
    std::string out;
    out.reserve(bytes.size());
    std::uint32_t cp = 0;
    int needed = 0, seen = 0;
    unsigned lower = 0x80, upper = 0xBF;
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto b = static_cast<unsigned char>(bytes[i]);
        if (needed == 0) {
            ++i;
            if (b <= 0x7F) {
                out.push_back(static_cast<char>(b));
            } else if (b >= 0xC2 && b <= 0xDF) {
                needed = 1;
                cp = b & 0x1F;
            } else if (b >= 0xE0 && b <= 0xEF) {
                if (b == 0xE0) lower = 0xA0;
                if (b == 0xED) upper = 0x9F;
                needed = 2;
                cp = b & 0xF;
            } else if (b >= 0xF0 && b <= 0xF4) {
                if (b == 0xF0) lower = 0x90;
                if (b == 0xF4) upper = 0x8F;
                needed = 3;
                cp = b & 0x7;
            } else {
                str::append_utf8(out, 0xFFFD);
            }
            continue;
        }
        if (b < lower || b > upper) {
            // この byte は読み直す (消費しない)
            cp = 0;
            needed = seen = 0;
            lower = 0x80;
            upper = 0xBF;
            str::append_utf8(out, 0xFFFD);
            continue;
        }
        ++i;
        lower = 0x80;
        upper = 0xBF;
        cp = (cp << 6) | (b & 0x3F);
        if (++seen == needed) {
            str::append_utf8(out, cp);
            cp = 0;
            needed = seen = 0;
        }
    }
    if (needed != 0) str::append_utf8(out, 0xFFFD);
    return out;
}

std::string run_spec_extract_sidecar(const std::string& bytes, const std::string& file_name, const std::string& label) {
    Json input = Json::object();
    input["fileName"] = file_name;
    input["label"] = label;
    input["base64"] = crypto::base64_encode(bytes);
    sidecar::Options o;
    o.timeout = std::chrono::milliseconds(120'000);
    o.max_output = 32u << 20;
    const auto r = sidecar::run("spec-extract.ts", input, o);
    if (!r.ok) fail(r.error);
    if (!r.result.is_object() || !r.result.contains("text") || !r.result["text"].is_string())
        fail(label + ": sidecar returned no text");
    return r.result["text"].get<std::string>();
}

std::optional<GithubSpecSource> parse_github_spec_url(std::string_view input) {
    if (auto ssh = parse_github_ssh_url(input)) return ssh;
    const auto u = url::parse(str::trim(input));
    if (!u || (u->scheme != "http" && u->scheme != "https")) return std::nullopt;
    const std::string& host = u->host;
    const auto parts = path_parts(u->path);
    if (host == "raw.githubusercontent.com" && parts.size() >= 4)
        return GithubSpecSource{parts[0], strip_git_suffix(parts[1]), join_from(parts, 3), parts[2]};
    if (host == "api.github.com" && !parts.empty() && parts[0] == "repos" && parts.size() >= 4) {
        GithubSpecSource s{parts[1], strip_git_suffix(parts[2]), std::nullopt, std::nullopt};
        const auto ci = std::find(parts.begin(), parts.end(), "contents");
        if (ci != parts.end()) s.path = join_from(parts, static_cast<std::size_t>(ci - parts.begin()) + 1);
        auto ref = query_get(*u, "ref");
        if (ref && !ref->empty()) s.ref = ref;
        return s;
    }
    if (host != "github.com" || parts.size() < 2) return std::nullopt;
    GithubSpecSource out{parts[0], strip_git_suffix(parts[1]), std::nullopt, std::nullopt};
    const std::string mode = parts.size() > 2 ? parts[2] : "";
    if ((mode == "blob" || mode == "raw") && parts.size() >= 5) {
        out.ref = parts[3];
        out.path = join_from(parts, 4);
    } else if (mode == "tree" && parts.size() >= 4) {
        out.ref = parts[3];
        const auto p = join_from(parts, 4);
        if (!p.empty()) out.path = p;
    }
    if (auto ref = query_get(*u, "ref"); ref && !ref->empty()) out.ref = ref;
    if (auto path = query_get(*u, "path"); path && !path->empty()) out.path = path;
    return out;
}

std::optional<GithubSpecSource> build_github_spec_source(std::string_view repo_url, const std::optional<std::string>& path,
                                                         const std::optional<std::string>& ref) {
    auto parsed = parse_github_spec_url(repo_url);
    if (!parsed) return std::nullopt;
    if (path && !str::trim(*path).empty()) parsed->path = str::trim(*path);
    if (ref && !str::trim(*ref).empty()) parsed->ref = str::trim(*ref);
    return parsed;
}

GithubRepoInfo fetch_github_repo_info(const GithubSpecSource& source, const ResolveSpecOpts& opts) {
    http::ClientRequest req;
    req.url = "https://api.github.com/repos/" + source.owner + "/" + source.repo;
    set_github_headers(req, opts.githubToken);
    req.max_response_bytes = 1u << 20;
    const auto res = do_fetch(opts, req);
    if (!res.ok())
        fail("GitHub repository read failed (HTTP " + std::to_string(res.status) + "): " + source.owner + "/" + source.repo);
    auto data = parse_json(res.body, {1u << 20, 100'000, 32});
    if (!data) fail("GitHub repository read failed (invalid JSON): " + source.owner + "/" + source.repo);
    GithubRepoInfo info{source.owner, source.repo, std::nullopt, std::nullopt, std::nullopt};
    info.isPrivate = json_opt_bool(*data, "private");
    info.defaultBranch = json_opt_string(*data, "default_branch");
    info.htmlUrl = json_opt_string(*data, "html_url");
    return info;
}

GithubSpecTextResult fetch_github_spec_text(const GithubSpecSource& source, const ResolveSpecOpts& opts) {
    if (!source.path || source.path->empty()) fail("GitHub URL requires a file path (blob URL or path field)");
    const std::string& path = *source.path;
    assert_supported_spec_file_name(path, "GitHub " + source.owner + "/" + source.repo + "/" + path);
    const std::size_t max_bytes = opts.maxBytes.value_or(DEFAULT_MAX_SPEC_BYTES);
    const std::string query = source.ref ? "?ref=" + url::encode_component(*source.ref) : "";
    http::ClientRequest req;
    req.url = "https://api.github.com/repos/" + source.owner + "/" + source.repo + "/contents/" + encode_github_path(path) + query;
    set_github_headers(req, opts.githubToken);
    // base64 + JSON の包みぶんの余裕を見た上限
    req.max_response_bytes = max_bytes / 3 * 4 + max_bytes / 38 + (256u << 10);
    const auto res = do_fetch(opts, req);
    if (!res.ok())
        fail("GitHub file read failed (HTTP " + std::to_string(res.status) + "): " + source.owner + "/" + source.repo + "/" + path);
    auto data = parse_json(res.body, {req.max_response_bytes, 100'000, 32});
    if (!data) fail("GitHub contents API did not return base64 file content: " + path);
    if (data->is_array()) fail("GitHub path is a directory, not a file: " + path);
    if (json_string(*data, "type") != "file") fail("GitHub path is not a file: " + path);
    const auto size_opt = json_opt_number(*data, "size");
    const double size = size_opt.value_or(0);
    if (size > static_cast<double>(max_bytes))
        fail("GitHub file is too large (" + js_number_to_string(size) + " > " + std::to_string(max_bytes) + " bytes): " + path);
    const std::string content = json_string(*data, "content");
    if (json_string(*data, "encoding") != "base64" || content.empty())
        fail("GitHub contents API did not return base64 file content: " + path);
    std::string compact;
    std::size_t i = 0;
    while (i < content.size()) {
        const std::size_t start = i;
        if (!str::is_js_space(str::next_codepoint(content, i))) compact.append(content, start, i - start);
    }
    const auto bytes = crypto::base64_decode(compact);
    if (!bytes) fail("GitHub contents API did not return base64 file content: " + path);
    GithubSpecTextResult out;
    out.text = extract_spec_text_from_bytes(*bytes, path, opts);
    out.source = source;
    out.size = size;
    out.sha = json_opt_string(*data, "sha");
    out.htmlUrl = json_opt_string(*data, "html_url");
    return out;
}

std::string fetch_spec_text(const std::string& url_text, const ResolveSpecOpts& opts) {
    const std::size_t max_bytes = opts.maxBytes.value_or(DEFAULT_MAX_SPEC_BYTES);
    http::ClientRequest req;
    req.url = url_text;
    req.max_response_bytes = max_bytes;
    req.timeout = std::chrono::milliseconds(30'000);
    req.allow_private_network = false;  // 利用者が指定する URL (SSRF 対策)
    const auto res = do_fetch(opts, req);
    if (!res.transport_ok) {
        if (looks_like_size_limit(res.error))
            fail("仕様書が大きすぎます (> " + std::to_string(max_bytes) + " bytes): " + url_text);
        fail("仕様書 URL の取得に失敗 (" + res.error + "): " + url_text);
    }
    if (!res.ok()) fail("仕様書 URL の取得に失敗 (HTTP " + std::to_string(res.status) + "): " + url_text);
    const std::string& buf = res.body;
    if (buf.size() > max_bytes)
        fail("仕様書が大きすぎます (" + std::to_string(buf.size()) + " > " + std::to_string(max_bytes) + " bytes): " + url_text);
    const std::string content_type = str::to_lower(res.headers.get("content-type").value_or(""));
    const auto parsed = url::parse(url_text);
    const std::string url_path = parsed ? parsed->path : url_text;
    const std::string ext = spec_ext(url_path);
    const std::string label = "URL " + url_text;
    if (!ext.empty() && !is_supported_spec_file_name(url_path)) assert_supported_spec_file_name(url_path, label);
    if (str::contains(content_type, "application/pdf") || is_pdf_ext(url_path)) return sidecar_extract(opts, buf, "spec.pdf", label);
    if (str::contains(content_type, "officedocument.wordprocessingml") || is_docx_ext(url_path))
        return sidecar_extract(opts, buf, "spec.docx", label);
    if (str::contains(content_type, "presentationml") || is_pptx_ext(url_path)) return sidecar_extract(opts, buf, "spec.pptx", label);
    if (str::contains(content_type, "spreadsheetml") || is_xlsx_ext(url_path)) return sidecar_extract(opts, buf, "spec.xlsx", label);
    if (is_doc_ext(url_path)) return sidecar_extract(opts, buf, "spec.doc", label);
    const std::string decoded = decode_textual(buf, label);
    return str::contains(content_type, "text/html") || is_html_ext(url_path) ? str::strip_html(decoded) : decoded;
}

std::string read_spec_text_from_file(const std::string& file_path, const ResolveSpecOpts& opts) {
    const std::string label = "ファイル " + file_path;
    assert_supported_spec_file_name(file_path, label);
    const std::size_t max_bytes = opts.maxBytes.value_or(DEFAULT_MAX_SPEC_BYTES);
    if (file_path.find('\0') != std::string::npos || !fs::exists(file_path)) fail("仕様書ファイルが見つかりません: " + file_path);
    const auto size = fs::file_size(file_path);
    if (size && *size > max_bytes)
        fail("仕様書が大きすぎます (" + std::to_string(*size) + " > " + std::to_string(max_bytes) + " bytes): " + file_path);
    auto raw = fs::read_file(file_path, max_bytes);  // 読む間に大きくなっても上限を超えて確保しない
    if (!raw) fail("仕様書が大きすぎます (> " + std::to_string(max_bytes) + " bytes): " + file_path);
    const std::string& buf = *raw;
    if (is_pdf_ext(file_path) || is_pdf_bytes(buf)) return sidecar_extract(opts, buf, "spec.pdf", label);
    if (is_html_ext(file_path)) return str::strip_html(decode_textual(buf, label));
    if (is_doc_ext(file_path)) return sidecar_extract(opts, buf, "spec.doc", label);
    if (is_docx_ext(file_path) || is_zip_bytes(buf)) {
        if (is_pptx_ext(file_path)) return sidecar_extract(opts, buf, "spec.pptx", label);
        if (is_xlsx_ext(file_path)) return sidecar_extract(opts, buf, "spec.xlsx", label);
        return sidecar_extract(opts, buf, "spec.docx", label);
    }
    return decode_textual(buf, label);
}

bool is_http_url(std::string_view s) {
    const std::string t = str::to_lower_ascii(str::trim(s));
    return str::starts_with(t, "http://") || str::starts_with(t, "https://");
}

std::string resolve_spec_text(const std::string& url_or_path, const ResolveSpecOpts& opts) {
    const std::string src = str::trim(url_or_path);
    if (src.empty()) fail("仕様書ソースが空です");
    if (auto github = parse_github_spec_url(src)) return fetch_github_spec_text(*github, opts).text;
    if (is_http_url(src)) return fetch_spec_text(src, opts);
    if (!opts.allowLocalPath) fail("この経路ではローカルパスからの読み込みは許可されていません (URL を指定してください)");
    return read_spec_text_from_file(src, opts);
}

std::string extract_spec_text_from_bytes(const std::string& bytes, const std::string& file_name, const ResolveSpecOpts& opts) {
    const std::string label = "ファイル " + file_name;
    assert_supported_spec_file_name(file_name, label);
    if (is_pdf_ext(file_name) || is_pdf_bytes(bytes)) return sidecar_extract(opts, bytes, "spec.pdf", label);
    if (is_pptx_ext(file_name)) return sidecar_extract(opts, bytes, "spec.pptx", label);
    if (is_xlsx_ext(file_name)) return sidecar_extract(opts, bytes, "spec.xlsx", label);
    if (is_doc_ext(file_name)) return sidecar_extract(opts, bytes, "spec.doc", label);
    if (is_html_ext(file_name)) return str::strip_html(decode_textual(bytes, label));
    if (is_docx_ext(file_name) || is_zip_bytes(bytes)) return sidecar_extract(opts, bytes, "spec.docx", label);
    return decode_textual(bytes, label);
}

Json to_json(const GithubSpecSource& s) {
    Json o = Json::object();
    o["owner"] = s.owner;
    o["repo"] = s.repo;
    if (s.path) o["path"] = *s.path;
    if (s.ref) o["ref"] = *s.ref;
    return o;
}

} // namespace di::flow
