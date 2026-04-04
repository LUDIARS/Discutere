/**
 * Discutere — 認証ミドルウェア
 *
 * service_token の検証、または開発モードでのヘッダーフォールバック。
 */
import type { Context } from "hono";

export function getUserId(c: Context): string {
  return (c.get("userId") as string) || c.req.header("X-User-Id") || "";
}

export function getUserRole(c: Context): string {
  return (c.get("userRole") as string) || c.req.header("X-User-Role") || "general";
}
