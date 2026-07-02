/**
 * /api/admin/blackbox — 成長型ブラックボックスのレビュー & ルール管理 (admin)。
 *
 * 情報密度判定 (flow.information_density) 等の blackbox 判断に OK/NG を返し、
 * ルールの trial → auto 卒業 / 撤回を進める。実体は @ludiars/blackbox。
 *
 *   GET  /admin/blackbox/decisions          レビュー待ちキュー
 *   POST /admin/blackbox/decisions/:id/verdict  { verdict: "ok"|"ng" }
 *   GET  /admin/blackbox/rules?domain=      ルール一覧 + 卒業メトリクス
 *   POST /admin/blackbox/rules/:id/state    { state } 手動昇格/撤回/復活
 */

import { Hono } from "hono";
import type { Context } from "hono";
import { describeCondition, type BlackBox, type RuleState } from "@ludiars/blackbox";
import { getUserId, getUserRole } from "../middleware/auth.js";
import { getFlowDb } from "../flow/db/connection.js";
import { makeDensityBlackBox, DOMAIN_DENSITY } from "../flow/density-blackbox.js";

const RULE_STATES: RuleState[] = ["candidate", "trial", "auto", "retired"];

/** blackbox 判断を持つ既知ドメイン (一覧の既定走査対象)。 */
const KNOWN_DOMAINS = [DOMAIN_DENSITY];

function requireAdmin(c: Context): Response | null {
  const userId = getUserId(c);
  const role = getUserRole(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  if (role !== "admin") return c.json({ error: "admin role required" }, 403);
  return null;
}

let bbInstance: BlackBox | null = null;
function bb(): BlackBox {
  if (!bbInstance) bbInstance = makeDensityBlackBox(getFlowDb());
  return bbInstance;
}

/** テスト用: インスタンスをリセット (DB 差し替えに追従)。 */
export function _resetBlackboxRoutes(): void {
  bbInstance = null;
}

export const blackboxRoutes = new Hono();

blackboxRoutes.get("/admin/blackbox/decisions", (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const domain = c.req.query("domain") || undefined;
  const limit = Math.min(200, Math.max(1, Number(c.req.query("limit")) || 50));
  return c.json({ items: bb().ledger.listPending(domain, limit) });
});

blackboxRoutes.post("/admin/blackbox/decisions/:id/verdict", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const id = Number(c.req.param("id"));
  if (!Number.isInteger(id)) return c.json({ error: "invalid id" }, 400);
  const body = await c.req.json<{ verdict?: unknown }>().catch(() => ({ verdict: undefined }));
  if (body.verdict !== "ok" && body.verdict !== "ng") {
    return c.json({ error: "verdict must be 'ok' or 'ng'" }, 400);
  }
  const res = bb().engine.recordVerdict(id, body.verdict);
  if (!res.ok) return c.json({ error: "decision not found" }, 404);
  return c.json({ ok: true, rule: res.ruleUpdated ?? null });
});

blackboxRoutes.get("/admin/blackbox/rules", (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const domain = c.req.query("domain");
  const domains = domain ? [domain] : KNOWN_DOMAINS;
  const rules = domains.flatMap((d) => bb().engine.listRules(d)).map((rule) => ({
    ...rule,
    whenText: describeCondition(rule.when),
  }));
  const stats = domains.map((d) => bb().stats(d));
  return c.json({ rules, stats });
});

blackboxRoutes.post("/admin/blackbox/rules/:id/state", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const id = c.req.param("id");
  const body = await c.req.json<{ state?: unknown }>().catch(() => ({ state: undefined }));
  if (typeof body.state !== "string" || !RULE_STATES.includes(body.state as RuleState)) {
    return c.json({ error: `state must be one of ${RULE_STATES.join(",")}` }, 400);
  }
  const rule = bb().engine.setRuleState(id, body.state as RuleState);
  if (!rule) return c.json({ error: "rule not found" }, 404);
  return c.json({ ok: true, rule });
});
