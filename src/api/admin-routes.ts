/**
 * Admin API (PR-B) — 人間 → AI への介入経路。
 *
 * - POST /api/admin/rules/enabled  { enabled: boolean }
 *     engine.setRulesEnabled() を叩いて、 議論自走を runtime で止める/再開
 * - POST /api/admin/session/reset  { sessionId: string }
 *     engine.resetSession() を叩いて、 safety cap を解放
 * - GET  /api/admin/status
 *     現在の runtime kill switch 状態と直近 rule_log 件数を返す
 *
 * 認証: admin role 必須 (Cernere middleware の getUserRole)。
 * persona-engine インスタンスは src/index.ts で setPersonaEngine() で注入する。
 */

import { Hono } from "hono";
import type { Context } from "hono";

import { getUserId, getUserRole } from "../middleware/auth.js";
import type { PersonaEngineHandle } from "../persona-engine/index.js";

let engineInstance: PersonaEngineHandle | null = null;
let runtimeEnabled = true;

/** src/index.ts などから注入 (起動時 1 回) */
export function setPersonaEngine(engine: PersonaEngineHandle | null): void {
  engineInstance = engine;
}

export function getPersonaEngine(): PersonaEngineHandle | null {
  return engineInstance;
}

export const adminRoutes = new Hono();

adminRoutes.post("/admin/rules/enabled", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  if (!engineInstance) return c.json({ error: "persona-engine not initialized" }, 503);
  const body = await c.req.json<{ enabled: boolean }>().catch(() => ({ enabled: true }));
  const enabled = !!body.enabled;
  engineInstance.setRulesEnabled(enabled);
  runtimeEnabled = enabled;
  return c.json({ ok: true, enabled });
});

adminRoutes.post("/admin/session/reset", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  if (!engineInstance) return c.json({ error: "persona-engine not initialized" }, 503);
  const body = await c.req.json<{ sessionId: string }>().catch(() => ({ sessionId: "" }));
  if (!body.sessionId) return c.json({ error: "sessionId required" }, 400);
  engineInstance.resetSession(body.sessionId);
  return c.json({ ok: true, sessionId: body.sessionId });
});

adminRoutes.get("/admin/status", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  return c.json({
    persona_engine_attached: !!engineInstance,
    rules_runtime_enabled: runtimeEnabled,
    persona_count: engineInstance ? engineInstance.personas.list().length : 0,
    rule_count: engineInstance ? engineInstance.rules.list({ enabled: true }).length : 0,
    recent_rule_logs: engineInstance ? engineInstance.rules.recentLogs(10) : [],
  });
});

function requireAdmin(c: Context): Response | null {
  const userId = getUserId(c);
  const role = getUserRole(c);
  if (!userId) {
    return c.json({ error: "Authentication required" }, 401);
  }
  if (role !== "admin") {
    return c.json({ error: "admin role required" }, 403);
  }
  return null;
}
