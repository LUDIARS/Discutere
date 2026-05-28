import { EVENT_TYPES } from "../../../events/event-types.js";
import type { HandlerContext, HandlerResult } from "../types.js";

export function handleScene(ctx: HandlerContext): HandlerResult {
  const scene = ctx.args.join(" ").trim();
  if (!scene) return { ok: false, error: "usage: /scene <description>" };
  const s = ctx.core.repos.session.get(ctx.sessionId) as any;
  ctx.core.eventLog.appendEvent({ eventType: EVENT_TYPES.SessionUpdated, payload: { id: ctx.sessionId, workspaceId: ctx.workspaceId, title: s?.title ?? "session", startedAt: s?.started_at ?? Date.now(), endedAt: s?.ended_at ?? undefined, mode: "emotion", scene } as any });
  return { ok: true, message: "scene set" };
}

export function handleDone(ctx: HandlerContext): HandlerResult {
  const s = ctx.core.repos.session.get(ctx.sessionId) as any;
  ctx.core.eventLog.appendEvent({ eventType: EVENT_TYPES.SessionEnded, payload: { id: ctx.sessionId, workspaceId: ctx.workspaceId, title: s?.title ?? "session", startedAt: s?.started_at ?? Date.now(), endedAt: Date.now(), mode: s?.mode ?? "emotion", scene: s?.scene ?? undefined } as any });
  return { ok: true, message: "session ended" };
}
