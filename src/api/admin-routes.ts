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

// 未処理 (open) DesignGap を「却下」する terminal 状態 (status='dismissed')。
// dismissed は議論進行 (facilitator/engine)・キュー・結論一覧から除外される
// = データとして表示せず、学習にも乗せない。closed (収束した結論) とは区別する。
const DISMISS_GUARD = "status IS NULL OR status NOT IN ('closed','converged','dismissed')";

adminRoutes.post("/admin/gaps/:id/dismiss", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const id = c.req.param("id");
  const core = createCore();
  try {
    const r = core.client.raw
      .prepare(
        `UPDATE design_gaps SET status = 'dismissed', updated_at = ? WHERE id = ? AND workspace_id = ? AND (${DISMISS_GUARD})`
      )
      .run(Date.now(), id, getConfig().workspace);
    return c.json({ ok: true, dismissed: r.changes });
  } finally {
    core.close();
  }
});

// 未処理 DesignGap を一括却下 (バックログ一掃)。
adminRoutes.post("/admin/gaps/dismiss-open", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const core = createCore();
  try {
    const r = core.client.raw
      .prepare(
        `UPDATE design_gaps SET status = 'dismissed', updated_at = ? WHERE workspace_id = ? AND (status IS NULL OR status NOT IN ('closed','converged','dismissed','resolved','rejected'))`
      )
      .run(Date.now(), getConfig().workspace);
    return c.json({ ok: true, dismissed: r.changes });
  } finally {
    core.close();
  }
});

// 進行中の議論 session を「閉じる」。
// - 同じ Discord スレッド (scene) に紐づく session を全て ended_at で終了
//   (intake session + discussion-of-gap session が同一 scene を共有するため両方)。
// - その discussion-of-gap session に紐づく DesignGap を closed にし、 debate を止める
//   (findDiscussionSession が closed gap を弾くので tick rule が着地しなくなる)。
// 「却下 (dismissed)」がデータ/学習から外す破棄なのに対し、 こちらは
// 「議論を結論として終了する」 graceful close (closed は結論一覧に残る)。
adminRoutes.post("/admin/sessions/:id/close", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const id = c.req.param("id");
  const ws = getConfig().workspace;
  const core = createCore();
  try {
    const sess = core.client.raw
      .prepare("SELECT id, scene FROM sessions WHERE id = ? AND workspace_id = ?")
      .get(id, ws) as { id: string; scene: string | null } | undefined;
    if (!sess) return c.json({ error: "session not found" }, 404);
    const now = Date.now();

    // scene 共有 session を一括終了 (scene 無しは自身のみ)。
    const endedSessions = sess.scene
      ? core.client.raw
          .prepare(
            "UPDATE sessions SET ended_at = ?, updated_at = ? WHERE workspace_id = ? AND scene = ? AND ended_at IS NULL"
          )
          .run(now, now, ws, sess.scene).changes
      : core.client.raw
          .prepare("UPDATE sessions SET ended_at = ?, updated_at = ? WHERE id = ? AND ended_at IS NULL")
          .run(now, now, id).changes;

    // 終了した session 群に対応する gap を closed に (title から gapId 抽出)。
    const gapIds = (
      core.client.raw
        .prepare(
          `SELECT DISTINCT SUBSTR(title, LENGTH('discussion-of-gap:') + 1) AS gap_id
             FROM sessions
            WHERE workspace_id = ? AND title LIKE 'discussion-of-gap:%'
              AND ${sess.scene ? "scene = ?" : "id = ?"}`
        )
        .all(ws, sess.scene ?? id) as Array<{ gap_id: string }>
    ).map((r) => r.gap_id);

    const closeGap = core.client.raw.prepare(
      `UPDATE design_gaps SET status = 'closed', updated_at = ?
         WHERE id = ? AND workspace_id = ?
           AND (status IS NULL OR status NOT IN ('closed','converged','dismissed','resolved','rejected'))`
    );
    let closedGaps = 0;
    for (const gid of gapIds) closedGaps += closeGap.run(now, gid, ws).changes;

    return c.json({ ok: true, endedSessions, closedGaps });
  } finally {
    core.close();
  }
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
