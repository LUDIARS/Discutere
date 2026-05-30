/**
 * Discutere — 認証ミドルウェア (Discord-only / Cernere 非依存)
 *
 * Discord-only pivot により Cernere Composite 認証 / 独自 JWT 検証層は撤去した。
 * 実際の認可境界は **Discord Gateway (bot token 認証 + admin-id allowlist、
 * `src/discord-hook/gateway.ts`)**。HTTP 側の MACHINA admin REST はこの薄い
 * middleware が `X-User-Id` / `X-User-Role` ヘッダーだけを読む過渡的な介入経路。
 *
 * 注意: ヘッダーを信頼するので HTTP admin REST を公開ネットワークに晒さないこと
 * (ops 側で loopback / VPN 等に閉じる)。canonical な設計根拠は CLAUDE.md 参照。
 */

import type { Context } from "hono";
import { createMiddleware } from "hono/factory";

function setAnonymous(c: Context): void {
  c.set("userId" as never, "anonymous" as never);
  c.set("userRole" as never, "general" as never);
}

/** すべての /api ルートに適用する。X-User-Id / X-User-Role ヘッダーから context を作る */
export function userContext() {
  return createMiddleware(async (c, next) => {
    const headerUserId = c.req.header("X-User-Id");
    const headerRole = c.req.header("X-User-Role");
    if (headerUserId) {
      c.set("userId" as never, headerUserId as never);
      c.set("userRole" as never, (headerRole ?? "general") as never);
    } else {
      setAnonymous(c);
    }
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

export function getUserName(c: Context): string {
  return (c.get("userName") as string) || "";
}

export function getUserEmail(c: Context): string | null {
  const v = c.get("userEmail") as string | null | undefined;
  return v ?? null;
}
