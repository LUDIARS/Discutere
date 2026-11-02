#pragma once
// src/flow/web/spec-extract-route.ts の移植: 仕様書バイナリアップロード + テキスト抽出エンドポイント (Web ファイルアップロード用)。
//
//   POST /api/spec/extract        multipart/form-data { file } → { ok: true, text }
//   GET  /api/spec/github/auth    → { ok: true, auth }
//   POST /api/spec/github/check   { repoUrl, path?, ref? } → 読めるかの確認 (+ 本文)
//
// loopback 信頼の Web UI がバイナリファイル (PDF/DOCX) を送信し、サーバ側でテキストを抽出して返す。
// PDF/DOCX 等の抽出は flow/spec_source (Node サイドカー経由)。
// multipart の読み取りはこのモジュール内の最小の実装 (上限つき・線形)。本文全体は DEFAULT_MAX_SPEC_BYTES + 1MiB まで。
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "flow/github_cli.hpp"
#include "flow/spec_source.hpp"
#include "infra/http.hpp"

namespace di::flow::web {

/** /api/spec/extract の本文上限 (ファイル上限 + multipart の見出し等の余裕)。 */
inline constexpr std::size_t kSpecExtractMaxBody = DEFAULT_MAX_SPEC_BYTES + (1u << 20);
/** /api/spec/github/check の本文上限。 */
inline constexpr std::size_t kSpecGithubCheckMaxBody = 64 * 1024;

struct MultipartLimits {
    std::size_t max_parts = 32;
    std::size_t max_header_bytes = 8 * 1024;
};

struct MultipartPart {
    std::string name;
    /** filename 属性があればファイル (空文字もファイル)。 */
    std::optional<std::string> filename;
    std::string contentType;
    std::string data;
};

/** Content-Type の boundary (1..70 文字)。multipart/form-data でなければ nullopt。 */
std::optional<std::string> multipart_boundary(std::string_view content_type);

/** multipart/form-data を読む。壊れている・上限超過は nullopt。 */
std::optional<std::vector<MultipartPart>> parse_multipart(std::string_view body, std::string_view boundary, const MultipartLimits& limits = {});

struct SpecExtractDeps {
    SpecSidecarExtractor extractor;
    std::function<GitHubCliAuthStatus()> authStatus;
    std::function<std::optional<std::string>()> githubToken;
    std::function<http::ClientResponse(const http::ClientRequest&)> fetchImpl;
};

http::Router spec_extract_routes(SpecExtractDeps deps = {});

} // namespace di::flow::web
