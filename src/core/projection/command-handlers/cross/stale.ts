import type { HandlerContext, HandlerResult } from "../types.js";

export function handleStale(ctx: HandlerContext): HandlerResult {
  const rows = ctx.core.client.raw.prepare("SELECT id,statement,status FROM hypotheses WHERE workspace_id = ? AND stale_flagged = 1 ORDER BY updated_at DESC").all(ctx.workspaceId) as any[];
  if (!rows.length) return { ok: true, message: "no stale hypotheses" };
  return { ok: true, message: rows.map((r) => `- ${r.id} (${r.status}): ${r.statement}`).join("\n") };
}
