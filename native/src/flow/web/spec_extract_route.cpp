#include "flow/web/spec_extract_route.hpp"

#include <exception>

#include <lorica/input_budget.hpp>

#include "base/json.hpp"
#include "base/strings.hpp"

namespace di::flow::web {
namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lower_str(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = lower(c);
    return out;
}

std::string trim_ows(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return std::string(s.substr(b, e - b));
}

/** "; k=v; k2="v 2"" の並びを読む (引用符の中の ; と \" を扱う)。最初の要素 (型) は返さない。 */
std::vector<std::pair<std::string, std::string>> header_params(std::string_view value) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = value.find(';');
    while (i != std::string_view::npos && i < value.size()) {
        ++i;
        while (i < value.size() && (value[i] == ' ' || value[i] == '\t')) ++i;
        const std::size_t name_start = i;
        while (i < value.size() && value[i] != '=' && value[i] != ';') ++i;
        std::string name = lower_str(trim_ows(value.substr(name_start, i - name_start)));
        std::string v;
        if (i < value.size() && value[i] == '=') {
            ++i;
            while (i < value.size() && (value[i] == ' ' || value[i] == '\t')) ++i;
            if (i < value.size() && value[i] == '"') {
                ++i;
                while (i < value.size() && value[i] != '"') {
                    if (value[i] == '\\' && i + 1 < value.size()) ++i;
                    v.push_back(value[i]);
                    ++i;
                }
                if (i < value.size()) ++i;  // 閉じ引用符
                while (i < value.size() && value[i] != ';') ++i;
            } else {
                const std::size_t vs = i;
                while (i < value.size() && value[i] != ';') ++i;
                v = trim_ows(value.substr(vs, i - vs));
            }
        }
        if (!name.empty()) out.emplace_back(std::move(name), std::move(v));
        if (i >= value.size()) break;
        if (value[i] != ';') i = value.find(';', i);
        else continue;
    }
    return out;
}

std::optional<std::string> param(const std::vector<std::pair<std::string, std::string>>& params, const std::string& name) {
    for (const auto& [k, v] : params)
        if (k == name) return v;
    return std::nullopt;
}

Json error_json(const std::string& message) { return Json{{"ok", false}, {"error", message}}; }

Json auth_json(const GitHubCliAuthStatus& a) {
    return Json{{"available", a.available}, {"authenticated", a.authenticated}, {"output", a.output}};
}

Json repo_json(const GithubRepoInfo& r) {
    Json o = Json::object();
    o["owner"] = r.owner;
    o["repo"] = r.repo;
    if (r.isPrivate) o["private"] = *r.isPrivate;
    if (r.defaultBranch) o["defaultBranch"] = *r.defaultBranch;
    if (r.htmlUrl) o["htmlUrl"] = *r.htmlUrl;
    return o;
}

/** typeof body.x === "string" ? body.x.trim() : "" */
std::string body_string(const Json& body, const char* key) {
    return body.is_object() && body.contains(key) && body[key].is_string() ? str::trim(body[key].get<std::string>()) : "";
}

} // namespace

std::optional<std::string> multipart_boundary(std::string_view content_type) {
    const std::size_t semi = content_type.find(';');
    if (lower_str(trim_ows(content_type.substr(0, semi))) != "multipart/form-data") return std::nullopt;
    auto b = param(header_params(content_type), "boundary");
    if (!b || b->empty() || b->size() > 70) return std::nullopt;
    return b;
}

std::optional<std::vector<MultipartPart>> parse_multipart(std::string_view body, std::string_view boundary, const MultipartLimits& limits) {
    if (boundary.empty() || boundary.size() > 70) return std::nullopt;
    lorica::input_budget budget({body.size(), limits.max_parts, limits.max_parts, 1});
    const std::string dash = "--" + std::string(boundary);
    const std::string delim = "\r\n" + dash;
    // 最初の区切り (前置きは捨てる。区切りは本文の先頭か CRLF の直後)。
    std::size_t pos;
    if (body.compare(0, dash.size(), dash) == 0) pos = dash.size();
    else {
        const std::size_t f = body.find(delim);
        if (f == std::string_view::npos) return std::nullopt;
        pos = f + delim.size();
    }
    std::vector<MultipartPart> parts;
    while (true) {
        if (body.compare(pos, 2, "--") == 0) return parts;  // 終端
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;  // transport padding
        if (body.compare(pos, 2, "\r\n") != 0) return std::nullopt;
        pos += 2;
        const std::size_t header_end = body.find("\r\n\r\n", pos);
        std::size_t headers_len;
        if (body.compare(pos, 2, "\r\n") == 0) headers_len = 0;  // 見出しなし
        else if (header_end == std::string_view::npos) return std::nullopt;
        else headers_len = header_end - pos;
        if (headers_len > limits.max_header_bytes) return std::nullopt;
        if (!budget.consume(0, 1, 1)) return std::nullopt;
        MultipartPart part;
        bool has_disposition = false;
        for (const auto& line : str::split(body.substr(pos, headers_len), "\r\n")) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) return std::nullopt;
            const std::string name = lower_str(trim_ows(std::string_view(line).substr(0, colon)));
            const std::string value = trim_ows(std::string_view(line).substr(colon + 1));
            if (name == "content-disposition") {
                const std::size_t semi = value.find(';');
                if (lower_str(trim_ows(std::string_view(value).substr(0, semi))) != "form-data") return std::nullopt;
                const auto params = header_params(value);
                auto n = param(params, "name");
                if (!n) return std::nullopt;
                part.name = *n;
                part.filename = param(params, "filename");
                has_disposition = true;
            } else if (name == "content-type") {
                part.contentType = value;
            }
        }
        if (!has_disposition) return std::nullopt;
        pos += headers_len + (headers_len == 0 ? 2 : 4);
        const std::size_t end = body.find(delim, pos);
        if (end == std::string_view::npos) return std::nullopt;  // 閉じていない
        if (!budget.consume(end - pos, 0, 0)) return std::nullopt;
        part.data = std::string(body.substr(pos, end - pos));
        parts.push_back(std::move(part));
        pos = end + delim.size();
    }
}

http::Router spec_extract_routes(SpecExtractDeps deps) {
    http::Router r;

    r.post(
        "/api/spec/extract",
        [deps](http::Request& c) {
            const std::string content_type = c.header("content-type").value_or("");
            const std::string kind = lower_str(trim_ows(std::string_view(content_type).substr(0, content_type.find(';'))));
            std::optional<std::vector<MultipartPart>> parts;
            if (kind == "application/x-www-form-urlencoded") {
                parts.emplace();  // フォームは読めるが file はファイルにならない (string)
            } else if (auto boundary = multipart_boundary(content_type)) {
                parts = parse_multipart(c.body, *boundary);
            }
            if (!parts) return http::Response::json(error_json("multipart/form-data が必要です"), 400);

            const MultipartPart* file = nullptr;
            for (const auto& p : *parts)
                if (p.name == "file") {
                    file = &p;
                    break;
                }
            if (!file || !file->filename) return http::Response::json(error_json("file フィールドが必要です"), 400);

            if (file->data.size() > DEFAULT_MAX_SPEC_BYTES)
                return http::Response::json(error_json("ファイルが大きすぎます (" + std::to_string(file->data.size()) + " > " +
                                                       std::to_string(DEFAULT_MAX_SPEC_BYTES) + " bytes)"),
                                            413);
            const std::string file_name = file->filename->empty() ? "upload" : *file->filename;
            if (!is_supported_spec_file_name(file_name))
                return http::Response::json(error_json("unsupported file format: " + file_name), 415);
            try {
                ResolveSpecOpts opts;
                opts.extractor = deps.extractor;
                opts.fetchImpl = deps.fetchImpl;
                const std::string text = extract_spec_text_from_bytes(file->data, file_name, opts);
                return http::Response::json(Json{{"ok", true}, {"text", text}});
            } catch (const std::exception& e) {
                return http::Response::json(error_json(e.what()), 422);
            }
        },
        {kSpecExtractMaxBody});

    r.get("/api/spec/github/auth", [deps](http::Request&) {
        const auto auth = deps.authStatus ? deps.authStatus() : get_github_cli_auth_status();
        return http::Response::json(Json{{"ok", true}, {"auth", auth_json(auth)}});
    });

    r.post(
        "/api/spec/github/check",
        [deps](http::Request& c) {
            const Json body = c.json(JsonLimits{kSpecGithubCheckMaxBody, 1000, 16}).value_or(Json::object());
            const std::string repo_url = body_string(body, "repoUrl");
            const std::string path = body_string(body, "path");
            const std::string ref = body_string(body, "ref");
            if (repo_url.empty()) return http::Response::json(error_json("repoUrl is required"), 400);
            const auto source = build_github_spec_source(repo_url, path, ref);
            if (!source) return http::Response::json(error_json("GitHub repository URL could not be parsed"), 400);

            const auto auth = deps.authStatus ? deps.authStatus() : get_github_cli_auth_status();
            const auto token = deps.githubToken ? deps.githubToken() : get_github_cli_token();
            ResolveSpecOpts opts;
            opts.githubToken = token;
            opts.fetchImpl = deps.fetchImpl;
            opts.extractor = deps.extractor;
            try {
                if (!source->path) {
                    const auto repo = fetch_github_repo_info(*source, opts);
                    return http::Response::json(Json{{"ok", true}, {"readable", true}, {"auth", auth_json(auth)}, {"repo", repo_json(repo)},
                                                     {"source", to_json(*source)}});
                }
                const auto result = fetch_github_spec_text(*source, opts);
                Json out = Json::object();
                out["ok"] = true;
                out["readable"] = true;
                out["auth"] = auth_json(auth);
                out["source"] = to_json(result.source);
                out["size"] = result.size;
                if (result.sha) out["sha"] = *result.sha;
                if (result.htmlUrl) out["htmlUrl"] = *result.htmlUrl;
                out["text"] = result.text;
                out["preview"] = str::js_slice(result.text, 0, 1000);
                return http::Response::json(out);
            } catch (const std::exception& e) {
                return http::Response::json(Json{{"ok", false}, {"readable", false}, {"auth", auth_json(auth)}, {"source", to_json(*source)},
                                                 {"error", e.what()}},
                                            422);
            }
        },
        {kSpecGithubCheckMaxBody});

    return r;
}

} // namespace di::flow::web
