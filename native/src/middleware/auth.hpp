#pragma once
// 認証ミドルウェア (src/middleware/auth.ts の移植。Discord-only / Cernere 非依存)。
//
// 実際の認可境界は Discord Gateway (bot token 認証 + admin-id allowlist)。HTTP 側の admin REST は
// この薄い middleware が X-User-Id / X-User-Role ヘッダーだけを読む過渡的な介入経路。
// ヘッダーを信頼するので HTTP admin REST を公開ネットワークに晒さないこと (loopback / VPN に閉じる)。
//
// 挙動 (TS と同じ。弱めも強めもしない):
//   - X-User-Id があれば userId=それ、userRole=X-User-Role (無ければ "general")
//   - 無ければ loopback (127.0.0.1 / ::1 / ::ffff:127.0.0.1) からは userId="loopback-admin", userRole="admin"
//   - それ以外は userId="anonymous", userRole="general"
// 空のヘッダー値は「無い」と同じに扱う (Hono の c.req.header と同じ)。
#include <optional>
#include <string>
#include <string_view>

#include "infra/http.hpp"

namespace di::middleware {

/** 要求が loopback (同一ホスト) からか (相手の生の IP を見る)。 */
bool is_loopback_request(const http::Request& request);

/** すべての /api ルートに適用する。req.vars に userId / userRole を入れる。 */
http::Middleware user_context();

/** 特定のロールを必須とするミドルウェア。違えば 403 {"error":"Role '<role>' required"}。 */
http::Middleware require_role(std::string role);

// ─── Context アクセサ ────────────────────────────────────────
std::string get_user_id(const http::Request& request);
std::string get_user_role(const http::Request& request);
std::string get_user_name(const http::Request& request);
std::optional<std::string> get_user_email(const http::Request& request);

/**
 * admin REST の共通 guard (dashboard-routes / errors-routes の requireAdmin)。
 * userId が空なら 401 {"error":"Authentication required"}、role≠admin なら 403 {"error":"admin role required"}。
 * 通ってよければ nullopt。
 */
std::optional<http::Response> require_admin(const http::Request& request);

} // namespace di::middleware
