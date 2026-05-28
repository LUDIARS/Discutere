import { EVENT_TYPES } from "../../../events/event-types.js";
import type { HandlerContext, HandlerResult } from "../types.js";

export function handleDefine(ctx: HandlerContext): HandlerResult {
  const name = ctx.args.join(" ").trim();
  if (!name) return { ok: false, error: "usage: /define <name>" };
  const id = ctx.core.repos.mechanic.create({ workspaceId: ctx.workspaceId, name });
  return { ok: true, message: `defined mechanic ${id}` };
}

export function handleIntends(ctx: HandlerContext): HandlerResult {
  const intends = ctx.args.join(" ").trim();
  if (!intends) return { ok: false, error: "usage: /intends <affect>" };
  const row = ctx.core.client.raw.prepare("SELECT id,name,description FROM mechanics WHERE workspace_id = ? ORDER BY updated_at DESC LIMIT 1").get(ctx.workspaceId) as any;
  if (!row) return { ok: false, error: "no mechanic to refine" };
  ctx.core.eventLog.appendEvent({ eventType: EVENT_TYPES.MechanicRefined, payload: { id: row.id, workspaceId: ctx.workspaceId, name: row.name, description: row.description, intends } as any });
  return { ok: true, message: "mechanic intent updated" };
}

export function handleRefine(ctx: HandlerContext): HandlerResult {
  const description = ctx.args.join(" ").trim();
  if (!description) return { ok: false, error: "usage: /refine <text>" };
  const row = ctx.core.client.raw.prepare("SELECT id,name,intends FROM mechanics WHERE workspace_id = ? ORDER BY updated_at DESC LIMIT 1").get(ctx.workspaceId) as any;
  if (!row) return { ok: false, error: "no mechanic to refine" };
  ctx.core.eventLog.appendEvent({ eventType: EVENT_TYPES.MechanicRefined, payload: { id: row.id, workspaceId: ctx.workspaceId, name: row.name, description, intends: row.intends ?? undefined } as any });
  return { ok: true, message: "mechanic refined" };
}

export function handleCompare(ctx: HandlerContext): HandlerResult {
  if (ctx.args.length < 2) return { ok: false, error: "usage: /compare <a> <b>" };
  return { ok: true, message: `compare stub: ${ctx.args[0]} vs ${ctx.args[1]}` };
}

export function handleHistory(ctx: HandlerContext): HandlerResult {
  const name = ctx.args.join(" ").trim();
  if (!name) return { ok: false, error: "usage: /history <mechanic>" };
  const rows = ctx.core.client.raw.prepare("SELECT id,description,intends FROM mechanics WHERE workspace_id = ? AND name = ? ORDER BY updated_at DESC").all(ctx.workspaceId, name) as any[];
  return { ok: true, message: `history count=${rows.length}` };
}
