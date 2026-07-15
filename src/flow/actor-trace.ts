import { getFlowDb } from "./db/connection.js";

export function associateCernereActor(
  sessionId: string,
  cernereUserId: string,
  associatedAt = Date.now(),
): void {
  const normalizedSessionId = sessionId.trim();
  const normalizedUserId = cernereUserId.trim();
  if (!normalizedSessionId) throw new Error("sessionId is required");
  if (!normalizedUserId) throw new Error("cernereUserId is required");
  getFlowDb().prepare(
    `INSERT INTO flow_actor_trace (session_id, cernere_user_id, source, associated_at)
     VALUES (?, ?, 'glab', ?)
     ON CONFLICT(session_id) DO UPDATE SET
       cernere_user_id = excluded.cernere_user_id,
       source = excluded.source,
       associated_at = excluded.associated_at`,
  ).run(normalizedSessionId, normalizedUserId, associatedAt);
}
