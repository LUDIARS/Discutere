import { randomUUID } from "node:crypto";
import type { ReturnTypeCreateCore } from "../../projection/types.js";

export function createManualGap(core: ReturnTypeCreateCore, input: {
  workspaceId: string;
  mechanicId: string;
  expectedAffect: string;
  observedAffect: string;
  evidenceUtteranceIds: string[];
}): string {
  if (!input.evidenceUtteranceIds.length) throw new Error("evidence is required");
  const id = randomUUID();
  core.client.raw.prepare(
    `INSERT INTO design_gaps (id, workspace_id, game_id, title, description, status, gap_in, expected_affect, observed_affect, evidence_json, created_at, updated_at)
     VALUES (?, ?, NULL, ?, ?, 'open', ?, ?, ?, ?, ?, ?)`
  ).run(
    id,
    input.workspaceId,
    `Manual Gap ${input.mechanicId}`,
    "manually created",
    input.mechanicId,
    input.expectedAffect,
    input.observedAffect,
    JSON.stringify(input.evidenceUtteranceIds),
    Date.now(),
    Date.now()
  );
  return id;
}
