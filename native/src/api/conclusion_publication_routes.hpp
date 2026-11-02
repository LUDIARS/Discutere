#pragma once
// src/api/conclusion-publication-routes.ts の移植: 議論結果の公開 (GLAB 向け読み取り) とローカル操作の公開設定。
//
// 安全の境界 (TS と同じ判定):
//   - 全応答に Cache-Control: private, no-store。
//   - /integrations/glab/conclusions*: 別建てのサーバ資格 (config.glabConclusions.token)。
//     32 バイト未満 (未設定を含む) なら 503 conclusions_unconfigured。Authorization: Bearer <token> を
//     定数時間で比較し、違えば 401。クライアントが送る audience / role ヘッダは一切信用しない。
//   - /admin/conclusion-publication*: ローカルの操作者だけ。相手が loopback かつ要求 URL のホスト名が
//     localhost / 127.0.0.1 / [::1] でなければ 403 local_operator_required。Origin があれば要求の origin と
//     一致しなければ 403 origin_denied (トンネル・プロキシ越し・別サイトのブラウザ送信を拒む)。
#include <functional>
#include <optional>
#include <string>

#include "base/sqlite.hpp"
#include "infra/http.hpp"

namespace di::api {

struct RequestUrlParts {
    /** new URL(c.req.url).hostname (小文字、IPv6 は角括弧つき)。 */
    std::string hostname;
    /** new URL(c.req.url).origin。 */
    std::string origin;
};

/**
 * 要求の URL (Host ヘッダ、または絶対形式の要求行) からホスト名と origin を取り出す (@hono/node-server と同じ組み立て)。
 * Host が無い・形が読めない (ポートが数字でない・65535 超・userinfo つき等) なら nullopt (呼び出し側は拒否する)。
 * テスト用に公開。
 */
std::optional<RequestUrlParts> request_url_parts(const http::Request& req);

/** Authorization ヘッダから /^Bearer (\S+)$/ の token を取り出す (無ければ空)。Node と同じく latin1 として読む。 */
std::string bearer_token_of(const std::optional<std::string>& authorization);

using DatabaseFn = std::function<sql::Database&()>;
using TokenFn = std::function<std::string()>;

/** 既定は flow DB と config.glabConclusions.token。 */
http::Router create_conclusion_publication_routes(DatabaseFn database = {}, TokenFn token = {});

} // namespace di::api
