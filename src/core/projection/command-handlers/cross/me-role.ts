import type { HandlerContext, HandlerResult } from "../types.js";

export function handleMeRole(ctx: HandlerContext): HandlerResult {
  const role = (ctx.args[0] ?? "").toLowerCase();
  if (!ctx.personId) return { ok: false, error: "personId is required" };
  if (!["theorist", "player", "developer"].includes(role)) return { ok: false, error: "usage: /me role <theorist|player|developer>" };

  const p = ctx.core.repos.person.get(ctx.personId) as any;
  if (p) {
    ctx.core.repos.person.update(ctx.personId, { workspaceId: ctx.workspaceId, name: p.name, role });
  } else {
    ctx.core.repos.person.create({ id: ctx.personId, workspaceId: ctx.workspaceId, name: ctx.personId, role });
  }
  return { ok: true, message: `role updated to ${role}` };
}
