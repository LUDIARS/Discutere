/**
 * Discutere — リクエストユーザコンテキスト (Cernere 撤去後の薄い実装)
 *
 * Cernere/JWT を撤去したため、 ここでは Discord-only 路線への移行期として
 * X-User-Id / X-User-Role ヘッダーをそのまま userContext に載せるだけのレイヤを残す。
 *
 * 本格的な認可は Di-1 (Discord Interactions endpoint + guild role guard) で
 * slash command 側に集約される予定。 REST admin API はそれまでの暫定。
 */

import type { Context } from "hono";
import { createMiddleware } from "hono/factory";

/**
 * すべての /api ルートに適用する. ヘッダーからユーザ情報を取り出して context に載せる.
 * 認証ロジック (Cookie / JWT / Cernere 交換) は持たない.
 */
export function userContext() {
  return createMiddleware(async (c, next) => {
    const userId = c.req.header("X-User-Id") || "anonymous";
    const userRole = c.req.header("X-User-Role") || "general";
    c.set("userId" as never, userId as never);
    c.set("userRole" as never, userRole as never);
    await next();
  });
}

/** 特定のロールを必須とするミドルウェア (admin 等) */
export function requireRole(role: string) {
  return createMiddleware(async (c, next) => {
    const current = (c.get("userRole" as never) as string) ?? "general";
    if (current !== role) {
      return c.json({ error: `Role '${role}' required` }, 403);
    }
    await next();
  });
}

// ─── Context アクセサ ────────────────────────────────────────

export function getUserId(c: Context): string {
  return (c.get("userId") as string) || c.req.header("X-User-Id") || "";
}

export function getUserRole(c: Context): string {
  return (c.get("userRole") as string) || c.req.header("X-User-Role") || "general";
}
