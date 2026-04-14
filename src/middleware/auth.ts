/**
 * Discutere — 認証ミドルウェア
 *
 * Cookie (discutere_token) または Bearer Token から service_token を取り出し、
 * Discutere 自身の JWT_SECRET で検証する。Cernere は認証プロバイダであり、
 * service_token の検証は Discutere 側で行う (LUDIARS 標準パターン)。
 *
 * 開発モードでは X-User-Id / X-User-Role ヘッダーでフォールバック可能。
 */

import type { Context } from "hono";
import { createMiddleware } from "hono/factory";
import { getCookie } from "hono/cookie";
import jwt from "jsonwebtoken";

const TOKEN_COOKIE = "discutere_token";

interface ServiceJwtPayload {
  sub?: string;
  userId?: string;
  name?: string;
  email?: string | null;
  role?: string;
}

function extractToken(c: Context): string | null {
  const auth = c.req.header("Authorization");
  if (auth?.startsWith("Bearer ")) return auth.slice(7);
  const cookieToken = getCookie(c, TOKEN_COOKIE);
  if (cookieToken) return cookieToken;
  return null;
}

function setAnonymous(c: Context): void {
  c.set("userId" as never, "anonymous" as never);
  c.set("userRole" as never, "general" as never);
}

/** すべての /api ルートに適用する。トークンを検証して context にユーザー情報を載せる */
export function userContext() {
  const isDev = process.env.NODE_ENV !== "production";
  const jwtSecret = process.env.JWT_SECRET ?? "";

  return createMiddleware(async (c, next) => {
    const token = extractToken(c);

    if (token && jwtSecret) {
      try {
        const payload = jwt.verify(token, jwtSecret) as ServiceJwtPayload;
        const userId = payload.sub ?? payload.userId;
        if (userId) {
          c.set("userId" as never, userId as never);
          c.set("userRole" as never, (payload.role ?? "general") as never);
          c.set("userName" as never, (payload.name ?? "") as never);
          c.set("userEmail" as never, (payload.email ?? null) as never);
        } else {
          setAnonymous(c);
        }
      } catch {
        setAnonymous(c);
      }
    } else if (isDev && !token) {
      // 開発環境: ヘッダーフォールバック
      const headerUserId = c.req.header("X-User-Id");
      const headerRole = c.req.header("X-User-Role");
      if (headerUserId) {
        c.set("userId" as never, headerUserId as never);
        c.set("userRole" as never, (headerRole ?? "general") as never);
      } else {
        setAnonymous(c);
      }
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
