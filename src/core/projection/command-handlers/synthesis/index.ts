import { randomUUID } from "node:crypto";
import { EVENT_TYPES } from "../../../events/event-types.js";
import type { HandlerContext, HandlerResult } from "../types.js";
import { applyLifecycle } from "../../../hypothesis/lifecycle-handler.js";

function latestHypothesis(ctx: HandlerContext): any | undefined {
  return ctx.core.client.raw.prepare("SELECT * FROM hypotheses WHERE workspace_id = ? ORDER BY updated_at DESC LIMIT 1").get(ctx.workspaceId) as any;
}

export function handleRefs(ctx: HandlerContext): HandlerResult {
  const refs = ctx.args.join(" ").split(",").map((x) => x.trim()).filter(Boolean);
  const h = latestHypothesis(ctx);
  if (!h) return { ok: false, error: "no hypothesis" };
  ctx.core.eventLog.appendEvent({ eventType: EVENT_TYPES.HypothesisUpdated, payload: { id: h.id, workspaceId: ctx.workspaceId, statement: h.statement, designGapId: h.design_gap_id ?? undefined, status: h.status ?? undefined, integrated: !!h.integrated, validatedByEmotion: !!h.validated_by_emotion, refs } as any });
  return { ok: true, message: "refs linked" };
}

export function handlePropose(ctx: HandlerContext): HandlerResult {
  const title = ctx.args.join(" ").trim();
  if (!title) return { ok: false, error: "usage: /propose <title>" };
  const id = randomUUID();
  ctx.core.eventLog.appendEvent({ eventType: EVENT_TYPES.HypothesisCreated, payload: { id, workspaceId: ctx.workspaceId, statement: title, status: "draft", integrated: false, validatedByEmotion: false, refs: [] } as any });
  return { ok: true, message: `hypothesis ${id}` };
}

export function handleAddresses(ctx: HandlerContext): HandlerResult {
  const gapId = ctx.args[0];
  if (!gapId) return { ok: false, error: "usage: /addresses <gap_id>" };
  const h = latestHypothesis(ctx);
  if (!h) return { ok: false, error: "no hypothesis" };
  ctx.core.eventLog.appendEvent({ eventType: EVENT_TYPES.HypothesisUpdated, payload: { id: h.id, workspaceId: ctx.workspaceId, statement: h.statement, designGapId: gapId, status: h.status ?? "draft", integrated: !!h.integrated, validatedByEmotion: !!h.validated_by_emotion, refs: JSON.parse(h.refs_json ?? "[]") } as any });
  return { ok: true, message: "addresses linked" };
}

export function handleJump(_ctx: HandlerContext): HandlerResult { return { ok: true, message: "jumped to axis1" }; }

export function handleSubmit(ctx: HandlerContext): HandlerResult {
  const h = latestHypothesis(ctx);
  if (!h) return { ok: false, error: "no hypothesis" };
  if (!h.design_gap_id) return { ok: false, error: "/addresses is required before /submit" };
  const out = applyLifecycle(ctx.core, h.id, "submit", { workspaceId: ctx.workspaceId, sessionId: ctx.sessionId });
  if (!out.ok) return { ok: false, error: out.error };
  return { ok: true, message: "submitted" };
}

export function handleValidate(ctx: HandlerContext): HandlerResult {
  const mode = (ctx.args[0] ?? "").toLowerCase();
  if (mode !== "theory" && mode !== "emotion") return { ok: false, error: "usage: /validate theory|emotion" };
  const h = latestHypothesis(ctx);
  if (!h) return { ok: false, error: "no hypothesis" };
  const out = applyLifecycle(ctx.core, h.id, mode === "theory" ? "validate_theory" : "validate_emotion", { workspaceId: ctx.workspaceId, sessionId: ctx.sessionId });
  if (!out.ok) return { ok: false, error: out.error };
  return { ok: true, message: `validated ${mode}` };
}

export function handleIntegrate(ctx: HandlerContext): HandlerResult {
  const h = latestHypothesis(ctx);
  if (!h) return { ok: false, error: "no hypothesis" };
  const out = applyLifecycle(ctx.core, h.id, "integrate", { workspaceId: ctx.workspaceId, sessionId: ctx.sessionId });
  if (!out.ok) return { ok: false, error: out.error };
  return { ok: true, message: "integrated" };
}

export function handleReject(ctx: HandlerContext): HandlerResult {
  const h = latestHypothesis(ctx);
  if (!h) return { ok: false, error: "no hypothesis" };
  const out = applyLifecycle(ctx.core, h.id, "reject", { workspaceId: ctx.workspaceId, sessionId: ctx.sessionId });
  if (!out.ok) return { ok: false, error: out.error };
  return { ok: true, message: "rejected" };
}
