import { randomUUID } from "node:crypto";
import type { ReturnTypeCreateCore } from "../../projection/types.js";
import type { LlmClient } from "./llm-client.js";
import { EVENT_TYPES } from "../../events/event-types.js";

export async function processUtteranceForTranslation(input: { core: ReturnTypeCreateCore; llm: LlmClient; utteranceId: string; workspaceId: string }): Promise<void> {
  const utterance = input.core.repos.utterance.get(input.utteranceId) as any;
  if (!utterance) return;
  const session = input.core.repos.session.get(utterance.session_id) as any;
  if (!session || session.mode !== "emotion") return;

  const affectRows = input.core.client.raw.prepare("SELECT mood FROM affects WHERE workspace_id = ? ORDER BY updated_at DESC LIMIT 5").all(input.workspaceId) as Array<{ mood: string }>;
  const mechRows = input.core.client.raw.prepare("SELECT name FROM mechanics WHERE workspace_id = ? ORDER BY updated_at DESC LIMIT 5").all(input.workspaceId) as Array<{ name: string }>;

  const result = await input.llm.extract(
    utterance.raw_content,
    affectRows.map((r) => r.mood),
    mechRows.map((r) => r.name),
  );

  const now = Date.now();
  if (result.affect) {
    const pid = randomUUID();
    input.core.client.raw.prepare(`INSERT INTO translation_proposals (id, workspace_id, utterance_id, proposal_type, target_name, confidence, pending_review, status, payload_json, created_at, updated_at)
      VALUES (?, ?, ?, 'affect', ?, ?, 1, 'pending', ?, ?, ?)`)
      .run(pid, input.workspaceId, input.utteranceId, result.affect.mood, result.affect.confidence, JSON.stringify(result.affect), now, now);
    input.core.eventLog.appendEvent({ eventType: EVENT_TYPES.TranslationProposed, payload: { id: pid, workspaceId: input.workspaceId, statement: `affect:${result.affect.mood}`, status: "pending" } as any });
  }
  if (result.mechanic) {
    const pid = randomUUID();
    input.core.client.raw.prepare(`INSERT INTO translation_proposals (id, workspace_id, utterance_id, proposal_type, target_name, confidence, pending_review, status, payload_json, created_at, updated_at)
      VALUES (?, ?, ?, 'mechanic', ?, ?, 1, 'pending', ?, ?, ?)`)
      .run(pid, input.workspaceId, input.utteranceId, result.mechanic.name, result.mechanic.confidence, JSON.stringify(result.mechanic), now, now);
    input.core.eventLog.appendEvent({ eventType: EVENT_TYPES.TranslationProposed, payload: { id: pid, workspaceId: input.workspaceId, statement: `mechanic:${result.mechanic.name}`, status: "pending" } as any });
  }
}
