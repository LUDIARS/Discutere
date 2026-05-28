import type { ReturnTypeCreateCore } from "../../projection/types.js";

export function existsGap(core: ReturnTypeCreateCore, input: {
  workspaceId: string;
  mechanicId: string;
  expectedAffect: string;
  observedAffect: string;
}): boolean {
  const row = core.client.raw.prepare(
    "SELECT id FROM design_gaps WHERE workspace_id = ? AND gap_in = ? AND expected_affect = ? AND observed_affect = ? AND status IN ('open','investigating') LIMIT 1"
  ).get(input.workspaceId, input.mechanicId, input.expectedAffect, input.observedAffect) as any;
  return !!row;
}
