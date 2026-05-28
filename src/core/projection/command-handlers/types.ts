import { randomUUID } from "node:crypto";
import type { ReturnTypeCreateCore } from "../types.js";
import { EVENT_TYPES } from "../../events/event-types.js";

export interface HandlerContext {
  core: ReturnTypeCreateCore;
  workspaceId: string;
  sessionId: string;
  personId?: string;
  args: string[];
}

export type HandlerResult = { ok: true; message: string } | { ok: false; error: string };

export function latestHypothesisId(core: ReturnTypeCreateCore, workspaceId: string): string | null {
  const rows = core.client.raw.prepare("SELECT id FROM hypotheses WHERE workspace_id = ? ORDER BY updated_at DESC LIMIT 1").all(workspaceId) as Array<{ id: string }>;
  return rows[0]?.id ?? null;
}

export function setSessionMode(core: ReturnTypeCreateCore, sessionId: string, workspaceId: string, mode: string, scene?: string): void {
  const s = core.repos.session.get(sessionId) as any;
  core.eventLog.appendEvent({
    eventType: EVENT_TYPES.SessionUpdated,
    payload: { id: sessionId, workspaceId, title: s?.title ?? "session", startedAt: s?.started_at ?? Date.now(), endedAt: s?.ended_at ?? undefined, mode, scene },
  });
}

export function appendEvent(core: ReturnTypeCreateCore, eventType: string, payload: any): void {
  core.eventLog.appendEvent({ id: randomUUID(), eventType: eventType as any, payload });
}
