import type { ReturnTypeCreateCore } from "../projection/types.js";
import { applyLifecycle } from "./lifecycle-handler.js";

export function finalizeValidationForSession(core: ReturnTypeCreateCore, workspaceId: string, sessionId: string): number {
  const rows = core.client.raw.prepare(
    "SELECT DISTINCT hypothesis_id FROM hypothesis_validations WHERE workspace_id = ? AND session_id = ?"
  ).all(workspaceId, sessionId) as Array<{ hypothesis_id: string }>;

  let moved = 0;
  for (const r of rows) {
    const out = applyLifecycle(core, r.hypothesis_id, "session_end", { workspaceId, sessionId });
    if (out.ok) moved += 1;
  }
  return moved;
}
