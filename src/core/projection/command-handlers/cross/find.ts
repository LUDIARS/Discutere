import type { HandlerContext, HandlerResult } from "../types.js";

export function handleFind(ctx: HandlerContext): HandlerResult {
  const q = ctx.args.join(" ").trim().toLowerCase();
  if (!q) return { ok: false, error: "usage: /find <query>" };

  const out: string[] = [];
  const mech = ctx.core.client.raw.prepare("SELECT id,name,description FROM mechanics WHERE workspace_id = ?").all(ctx.workspaceId) as any[];
  const aff = ctx.core.client.raw.prepare("SELECT id,mood FROM affects WHERE workspace_id = ?").all(ctx.workspaceId) as any[];
  const hyp = ctx.core.client.raw.prepare("SELECT id,statement,status FROM hypotheses WHERE workspace_id = ?").all(ctx.workspaceId) as any[];
  const utt = ctx.core.client.raw.prepare("SELECT id,raw_content FROM utterances WHERE workspace_id = ?").all(ctx.workspaceId) as any[];

  for (const m of mech) if (`${m.name} ${m.description ?? ""}`.toLowerCase().includes(q)) out.push(`mechanic:${m.id}:${m.name}`);
  for (const a of aff) if (`${a.mood}`.toLowerCase().includes(q)) out.push(`affect:${a.id}:${a.mood}`);
  for (const h of hyp) if (`${h.statement}`.toLowerCase().includes(q)) out.push(`hypothesis:${h.id}:${h.statement}`);
  for (const u of utt) if (`${u.raw_content}`.toLowerCase().includes(q)) out.push(`utterance:${u.id}`);

  return { ok: true, message: out.slice(0, 20).join("\n") || "no results" };
}
