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
import { createCore } from "../core/index.js";
import { getConfig } from "../config.js";
import { buildTopicOpinionSnapshot } from "../visualize/topic-opinions.js";

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

adminRoutes.get("/admin/learning/topics", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const config = getConfig();
  const limit = Number(c.req.query("limit") ?? 40);
  const opinionsPerTopic = Number(c.req.query("opinionsPerTopic") ?? 8);
  const core = createCore();
  try {
    return c.json(
      buildTopicOpinionSnapshot(core.client.raw, config.workspace, {
        limit,
        opinionsPerTopic,
      })
    );
  } finally {
    core.close();
  }
});

// PR-F: learning metrics — persona / rule 採用率
adminRoutes.get("/admin/metrics/personas", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  if (!engineInstance) return c.json({ error: "persona-engine not initialized" }, 503);
  const { personaMetrics } = await import("../persona-engine/index.js");
  // hypothesis status resolver は未提供 (Discatier core を持たないため engine 単体)。
  // status 不明として "unknown" を返す stub。 production では event-bridge 側で
  // status を埋め込む resolver を inject する。
  const metrics = personaMetrics(engineInstance.personas, engineInstance.rules, () => "unknown");
  return c.json({ metrics });
});

adminRoutes.get("/admin/metrics/rules", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  if (!engineInstance) return c.json({ error: "persona-engine not initialized" }, 503);
  const { ruleMetrics } = await import("../persona-engine/index.js");
  return c.json({ metrics: ruleMetrics(engineInstance.rules) });
});

// PR-J: Rule Viewer — 全 rule + 補正履歴
adminRoutes.get("/admin/rules", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  if (!engineInstance) return c.json({ error: "persona-engine not initialized" }, 503);
  const { SEED_RULE_IDS } = await import("../persona-engine/index.js");
  const includeRemoved = c.req.query("includeRemoved") === "1";
  const allRules = engineInstance.rules.list();
  const rules = includeRemoved ? allRules : allRules.filter((r) => r.enabled === 1);
  const annotated = rules.map((r) => ({
    id: r.id,
    description: r.description,
    trigger_type: r.trigger_type,
    tick_sec: r.tick_sec,
    event_kind: r.event_kind,
    target: r.target,
    cooldown_sec: r.cooldown_sec,
    last_fired_at: r.last_fired_at,
    enabled: r.enabled === 1,
    added_at: r.added_at,
    added_by: r.added_by,
    removed_at: r.removed_at,
    removed_by: r.removed_by,
    removed_reason: r.removed_reason,
    is_seed: SEED_RULE_IDS.has(r.id),
    is_ai_added: r.added_by.startsWith("ai:"),
    is_ai_removed: r.removed_by !== null && r.removed_by.startsWith("ai:"),
  }));
  return c.json({ rules: annotated });
});

adminRoutes.get("/admin/rules/:id/logs", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  if (!engineInstance) return c.json({ error: "persona-engine not initialized" }, 503);
  const id = c.req.param("id");
  const limit = Math.min(200, Math.max(1, Number(c.req.query("limit") ?? 50)));
  const all = engineInstance.rules.recentLogs(1000);
  const filtered = all.filter((l) => l.rule_id === id).slice(0, limit);
  return c.json({ rule_id: id, logs: filtered });
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
