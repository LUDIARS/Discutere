import { EVENT_TYPES } from "../events/event-types.js";
import type { ReturnTypeCreateCore } from "../projection/types.js";
import { canTransition, type HypothesisAction, type HypothesisState } from "./state-machine.js";

function row(core: ReturnTypeCreateCore, id: string) {
  return core.client.raw.prepare("SELECT * FROM hypotheses WHERE id = ?").get(id) as any;
}

function currentState(h: any): HypothesisState {
  return (h?.status ?? "draft") as HypothesisState;
}

export function applyLifecycle(core: ReturnTypeCreateCore, hypothesisId: string, action: HypothesisAction, extra: { workspaceId: string; sessionId?: string } = { workspaceId: "" }) {
  const h = row(core, hypothesisId);
  if (!h) return { ok: false as const, error: "hypothesis not found" };
  const tr = canTransition(currentState(h), action);
  if (!tr.ok) return tr;

  const payload = {
    id: h.id,
    workspaceId: extra.workspaceId || h.workspace_id,
    statement: h.statement,
    designGapId: h.design_gap_id ?? undefined,
    status: tr.to,
    integrated: tr.to === "integrated",
    validatedByEmotion: tr.to === "validated_by_emotion" || tr.to === "integrated",
    sessionId: extra.sessionId,
    refs: JSON.parse(h.refs_json ?? "[]"),
  } as any;

  const ev =
    action === "submit" ? EVENT_TYPES.HypothesisProposed :
    action === "integrate" ? EVENT_TYPES.HypothesisIntegrated :
    action === "reject" ? EVENT_TYPES.HypothesisRejected :
    action === "session_end" ? EVENT_TYPES.HypothesisValidated :
    EVENT_TYPES.HypothesisDebated;

  core.eventLog.appendEvent({ eventType: ev as any, payload });
  return { ok: true as const, to: tr.to };
}
