import type { ReturnTypeCreateCore } from "../projection/types.js";

export function flagStaleHypotheses(core: ReturnTypeCreateCore, workspaceId: string, now = Date.now()): string[] {
  const limit = now - 30 * 24 * 60 * 60 * 1000;
  const rows = core.client.raw.prepare(
    "SELECT id, statement, status, design_gap_id, refs_json, validated_by_emotion FROM hypotheses WHERE workspace_id = ? AND updated_at < ? AND status NOT IN ('integrated','rejected') AND stale_flagged = 0"
  ).all(workspaceId, limit) as any[];
  const ids: string[] = [];
  for (const h of rows) {
    ids.push(h.id);
    core.eventLog.appendEvent({ eventType: "HypothesisStaleFlagged" as any, payload: { id: h.id, workspaceId, statement: h.statement, status: h.status, designGapId: h.design_gap_id ?? undefined, staleFlagged: true, validatedByEmotion: !!h.validated_by_emotion, refs: JSON.parse(h.refs_json ?? "[]") } as any });
  }
  return ids;
}
