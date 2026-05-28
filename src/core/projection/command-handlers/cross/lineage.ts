import type { HandlerContext, HandlerResult } from "../types.js";

export function handleLineage(ctx: HandlerContext): HandlerResult {
  const mid = ctx.args[0];
  if (!mid) return { ok: false, error: "usage: /lineage <mechanic_id>" };

  const m = ctx.core.client.raw.prepare("SELECT id,name,description FROM mechanics WHERE id = ?").get(mid) as any;
  if (!m) return { ok: false, error: "mechanic not found" };
  const utterances = ctx.core.client.raw.prepare("SELECT id,raw_content,session_id FROM utterances WHERE workspace_id = ? AND raw_content LIKE ? ORDER BY created_at DESC LIMIT 10").all(ctx.workspaceId, `%${m.name}%`) as any[];
  const hyps = ctx.core.client.raw.prepare("SELECT id,statement,status FROM hypotheses WHERE workspace_id = ? AND statement LIKE ? ORDER BY updated_at DESC LIMIT 10").all(ctx.workspaceId, `%${m.name}%`) as any[];

  const lines: string[] = [
    `# Lineage ${m.name}`,
    `- mechanic: ${m.id}`,
    `- utterances: ${utterances.length}`,
    `- hypotheses: ${hyps.length}`,
  ];
  return { ok: true, message: lines.join("\n") };
}
