import type { ReturnTypeCreateCore } from "../../projection/types.js";

export function listPendingReviews(core: ReturnTypeCreateCore, workspaceId: string) {
  return core.client.raw.prepare(
    "SELECT * FROM translation_proposals WHERE workspace_id = ? AND pending_review = 1 ORDER BY created_at ASC"
  ).all(workspaceId) as any[];
}
