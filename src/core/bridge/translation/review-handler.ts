import { randomUUID } from "node:crypto";
import type { ReturnTypeCreateCore } from "../../projection/types.js";
import { EVENT_TYPES } from "../../events/event-types.js";

function mark(core: ReturnTypeCreateCore, id: string, status: string) {
  core.client.raw.prepare("UPDATE translation_proposals SET pending_review = 0, status = ?, updated_at = ? WHERE id = ?").run(status, Date.now(), id);
}

export function approveProposal(core: ReturnTypeCreateCore, proposalId: string): void {
  const p = core.client.raw.prepare("SELECT * FROM translation_proposals WHERE id = ?").get(proposalId) as any;
  if (!p) throw new Error("proposal not found");
  mark(core, proposalId, "approved");
  if (p.proposal_type === "affect") {
    core.repos.affect.create({ workspaceId: p.workspace_id, mood: p.target_name, score: p.confidence });
  } else if (p.proposal_type === "mechanic") {
    core.repos.mechanic.create({ workspaceId: p.workspace_id, name: p.target_name });
  }
  core.eventLog.appendEvent({ eventType: EVENT_TYPES.TranslationApproved, payload: { id: proposalId, workspaceId: p.workspace_id, statement: p.target_name, status: "approved" } as any });
}

export function reviseProposal(core: ReturnTypeCreateCore, proposalId: string, targetName: string): void {
  const p = core.client.raw.prepare("SELECT * FROM translation_proposals WHERE id = ?").get(proposalId) as any;
  if (!p) throw new Error("proposal not found");
  mark(core, proposalId, "revised");
  if (p.proposal_type === "affect") {
    core.eventLog.appendEvent({ eventType: EVENT_TYPES.AffectAdded, payload: { id: randomUUID(), workspaceId: p.workspace_id, mood: targetName, score: p.confidence } as any });
    core.repos.affect.create({ workspaceId: p.workspace_id, mood: targetName, score: p.confidence });
    core.client.raw.prepare("UPDATE affects SET vocabulary_status = 'proposed' WHERE workspace_id = ? AND mood = ?").run(p.workspace_id, targetName);
  } else {
    core.repos.mechanic.create({ workspaceId: p.workspace_id, name: targetName });
  }
  core.eventLog.appendEvent({ eventType: EVENT_TYPES.TranslationRevised, payload: { id: proposalId, workspaceId: p.workspace_id, statement: targetName, status: "revised" } as any });
}

export function rejectProposal(core: ReturnTypeCreateCore, proposalId: string): void {
  const p = core.client.raw.prepare("SELECT * FROM translation_proposals WHERE id = ?").get(proposalId) as any;
  if (!p) throw new Error("proposal not found");
  mark(core, proposalId, "rejected");
  core.client.raw.prepare("UPDATE utterances SET normalized_content = COALESCE(normalized_content,'') || ' [uncategorized]' WHERE id = ?").run(p.utterance_id);
  core.eventLog.appendEvent({ eventType: EVENT_TYPES.TranslationRejected, payload: { id: proposalId, workspaceId: p.workspace_id, statement: p.target_name, status: "rejected" } as any });
}
