import { randomUUID } from "node:crypto";
import type { ReturnTypeCreateCore } from "../../projection/types.js";
import { shouldDetectGap } from "./matcher.js";
import { existsGap } from "./dedup.js";

export function detectGaps(core: ReturnTypeCreateCore, workspaceId: string): string[] {
  const created: string[] = [];
  const mechanics = core.client.raw.prepare("SELECT id, name, intended_affect FROM mechanics WHERE workspace_id = ? AND intended_affect IS NOT NULL").all(workspaceId) as any[];

  for (const m of mechanics) {
    const observedRows = core.client.raw.prepare(
      `SELECT tp.target_name AS observed, u.id AS utterance_id
       FROM utterances u
       JOIN translation_proposals tp ON tp.utterance_id = u.id AND tp.proposal_type = 'affect' AND tp.status IN ('approved','revised')
       WHERE u.workspace_id = ?`
    ).all(workspaceId) as Array<{ observed: string; utterance_id: string }>;
    const observed = observedRows.map((r) => r.observed).filter(Boolean) as string[];
    if (observed.length < 3) continue;

    const decision = shouldDetectGap({ intended: m.intended_affect, observed });
    if (!decision.detect) continue;

    const observedAffect = decision.type === "missing" ? "(missing)" : observed[0];
    if (existsGap(core, { workspaceId, mechanicId: m.id, expectedAffect: m.intended_affect, observedAffect })) continue;

    const evidence = observedRows.slice(0, 3).map((r) => r.utterance_id);
    const id = randomUUID();
    core.client.raw.prepare(
      `INSERT INTO design_gaps (id, workspace_id, game_id, title, description, status, gap_in, expected_affect, observed_affect, evidence_json, created_at, updated_at)
       VALUES (?, ?, NULL, ?, ?, 'open', ?, ?, ?, ?, ?, ?)`
    ).run(
      id,
      workspaceId,
      `Gap: ${m.name}`,
      `detected by matcher (${decision.type})`,
      m.id,
      m.intended_affect,
      observedAffect,
      JSON.stringify(evidence),
      Date.now(),
      Date.now()
    );
    created.push(id);
  }
  return created;
}
