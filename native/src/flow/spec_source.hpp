#pragma once
// src/flow/spec-source.ts の移植: 仕様書ソース解決 (③ 仕様書を別途受け取って解析)。
// Web ファイルアップロード / Discord 添付 / URL・パス指定をテキストへ解決する。
//
// - テキスト系 (md/txt/json/yaml 等) と HTML はこのプロセスで読む。HTML のタグ除去は di::str::strip_html (線形時間。
//   移植前の stripHtml は正規表現の繰り返しで二乗時間だった)。
// - PDF (pdf-parse)・DOCX (mammoth)・XLSX/PPTX (ZIP 展開 + XML 抽出)・旧 DOC は自作しない。
//   Node のサイドカー native/sidecar/spec-extract.ts (移植前の抽出関数をそのまま呼ぶ。ZIP 展開の上限も同じ) に任せる。
// - Web (loopback 信頼) はローカルパス読みを許す。Discord はサーバのローカルファイルを読ませない (allowLocalPath=false)。
// - 利用者が指定する URL の取得は SSRF 対策つき (非公開アドレスへは繋がない)。
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"
#include "infra/http_client.hpp"

namespace di::flow {

/** 1 仕様書あたりの最大バイト数。 */
inline constexpr std::size_t DEFAULT_MAX_SPEC_BYTES = 5'000'000;
/** サイドカーへ渡すバイト列の上限 (アップロード経路でも確保の前に課す)。 */
inline constexpr std::size_t kMaxSpecSidecarInputBytes = 64u << 20;

const std::vector<std::string>& supported_spec_extensions();
bool is_supported_spec_file_name(std::string_view file_name);
/** path.extname(p).toLowerCase() と同じ。 */
std::string spec_ext(std::string_view file_path);

bool is_pdf_bytes(std::string_view bytes);
bool is_zip_bytes(std::string_view bytes);
/** テキストとして扱えるか (NUL バイトや制御文字の比率で弾く)。 */
bool is_textual(std::string_view bytes);
/** TextDecoder("utf-8") と同じ復号 (BOM を落とし、不正な並びは U+FFFD)。 */
std::string decode_utf8_like_textdecoder(std::string_view bytes);

/**
 * サイドカーで本文を抽出する関数 (テストで差し替える)。
 * file_name は抽出方式を決める名前 (spec.pdf / spec.docx / spec.pptx / spec.xlsx / spec.doc)、label はエラー文言の頭。
 * 失敗は std::runtime_error (文言は移植前と同じ "<label>: ...")。
 */
using SpecSidecarExtractor =
    std::function<std::string(const std::string& bytes, const std::string& file_name, const std::string& label)>;

/** 既定の抽出: native/sidecar/spec-extract.ts を引数配列で起動する (期限 120 秒・出力上限つき)。 */
std::string run_spec_extract_sidecar(const std::string& bytes, const std::string& file_name, const std::string& label);

struct ResolveSpecOpts {
    std::optional<std::size_t> maxBytes;
    /** ローカルパス読みを許可するか (Web=true / Discord=false)。 */
    bool allowLocalPath = false;
    std::function<http::ClientResponse(const http::ClientRequest&)> fetchImpl;
    SpecSidecarExtractor extractor;
    std::optional<std::string> githubToken;
};

struct GithubSpecSource {
    std::string owner;
    std::string repo;
    std::optional<std::string> path;
    std::optional<std::string> ref;
};

struct GithubSpecTextResult {
    GithubSpecSource source;  // path は必ずある
    std::string text;
    double size = 0;
    std::optional<std::string> sha;
    std::optional<std::string> htmlUrl;
};

struct GithubRepoInfo {
    std::string owner;
    std::string repo;
    std::optional<bool> isPrivate;
    std::optional<std::string> defaultBranch;
    std::optional<std::string> htmlUrl;
};

std::optional<GithubSpecSource> parse_github_spec_url(std::string_view input);
std::optional<GithubSpecSource> build_github_spec_source(std::string_view repo_url, const std::optional<std::string>& path = std::nullopt,
                                                         const std::optional<std::string>& ref = std::nullopt);
GithubRepoInfo fetch_github_repo_info(const GithubSpecSource& source, const ResolveSpecOpts& opts = {});
GithubSpecTextResult fetch_github_spec_text(const GithubSpecSource& source, const ResolveSpecOpts& opts = {});

/** URL から仕様書テキストを取得する (PDF/DOCX/PPTX/XLSX/DOC はサイドカー、それ以外はテキスト)。 */
std::string fetch_spec_text(const std::string& url, const ResolveSpecOpts& opts = {});
/** ローカルパスから仕様書テキストを読む。 */
std::string read_spec_text_from_file(const std::string& file_path, const ResolveSpecOpts& opts = {});
/** http(s):// で始まるか (前後の空白は無視)。 */
bool is_http_url(std::string_view s);
/** URL またはローカルパスを仕様書テキストに解決する (allowLocalPath=false ならローカルパスは拒否)。 */
std::string resolve_spec_text(const std::string& url_or_path, const ResolveSpecOpts& opts = {});
/** バイト列から仕様書テキストを抽出する (Web ファイルアップロード用)。 */
std::string extract_spec_text_from_bytes(const std::string& bytes, const std::string& file_name, const ResolveSpecOpts& opts = {});

Json to_json(const GithubSpecSource& s);

} // namespace di::flow
