/**
 * エラー可視化 API (FEATURE ②)。
 *
 * - GET  /admin/errors?limit=100  → { errors: BufferedError[] } (新しい順)
 * - POST /admin/errors/clear      → clearErrors + { ok: true }
 *
 * console-capture が拾った直近の console.error/warn を返す。 admin role 必須
 * (= 既存 admin-routes と同じ loopback=admin guard)。
 */

import { Hono } from "hono";
import type { Context } from "hono";

import { getUserId, getUserRole } from "../middleware/auth.js";
import { recentErrors, clearErrors } from "../observability/error-buffer.js";

export const errorsRoutes = new Hono();

errorsRoutes.get("/admin/errors", (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const limit = Math.max(1, Math.min(200, Number(c.req.query("limit") ?? 100)));
  return c.json({ errors: recentErrors(limit) });
});

errorsRoutes.post("/admin/errors/clear", (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  clearErrors();
  return c.json({ ok: true });
});

function requireAdmin(c: Context): Response | null {
  const userId = getUserId(c);
  const role = getUserRole(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  if (role !== "admin") return c.json({ error: "admin role required" }, 403);
  return null;
}
