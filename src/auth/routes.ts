/**
 * Discutere 認証ルート
 *
 * Cernere に認証を委譲し、Cernere から受け取った auth_code を
 * service_token (Discutere 独自 JWT) に交換する。
 *
 *   GET  /login-url             — Cernere ログイン URL を返す
 *   POST /exchange              — auth_code を service_token に交換 (Cookie 設定)
 *   POST /logout                — Cookie 削除
 *   GET  /me                    — 現在のユーザー情報
 */

import { Hono } from "hono";
import { setCookie, deleteCookie } from "hono/cookie";
import { getLoginUrl, exchangeAuthCode, isCompositeEnabled } from "./composite.js";
import { getUserId, getUserRole, getUserName, getUserEmail } from "../middleware/auth.js";

const TOKEN_COOKIE = "discutere_token";
const TOKEN_COOKIE_MAX_AGE = 900; // 15 分 (service_token 有効期限と同期)

function setTokenCookie(c: Parameters<typeof setCookie>[0], token: string) {
  const isProd = process.env.NODE_ENV === "production";
  setCookie(c, TOKEN_COOKIE, token, {
    httpOnly: true,
    secure: isProd,
    sameSite: "Lax",
    path: "/",
    maxAge: TOKEN_COOKIE_MAX_AGE,
  });
}

// ─── 認証不要ルート ──────────────────────────────────────────
export const compositeAuthRoutes = new Hono();

compositeAuthRoutes.get("/login-url", (c) => {
  if (!isCompositeEnabled()) {
    return c.json({ error: "Cernere Composite is not configured" }, 503);
  }
  const origin = c.req.query("origin");
  if (!origin) return c.json({ error: "origin query param required" }, 400);
  return c.json({ url: getLoginUrl(origin) });
});

compositeAuthRoutes.post("/exchange", async (c) => {
  const body = await c.req.json<{ code?: string }>();
  if (!body.code) return c.json({ error: "code is required" }, 400);

  try {
    const { serviceToken, user } = await exchangeAuthCode(body.code);
    setTokenCookie(c, serviceToken);
    return c.json({ user });
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    return c.json({ error: msg }, 401);
  }
});

compositeAuthRoutes.post("/logout", (c) => {
  deleteCookie(c, TOKEN_COOKIE, { path: "/" });
  return c.json({ ok: true });
});

// ─── 認証必須ルート ──────────────────────────────────────────
export const authRoutes = new Hono();

authRoutes.get("/me", (c) => {
  const userId = getUserId(c);
  if (!userId || userId === "anonymous") {
    return c.json({ error: "Authentication required" }, 401);
  }
  return c.json({
    id: userId,
    name: getUserName(c),
    email: getUserEmail(c),
    role: getUserRole(c),
  });
});
