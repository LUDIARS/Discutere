import type { ReturnTypeCreateCore } from "./types.js";

export function addReaction(input: { core: ReturnTypeCreateCore; workspaceId: string; utteranceId: string; actorId?: string; reactionType: string; intensity?: number }): string {
  return input.core.repos.reaction.create({
    workspaceId: input.workspaceId,
    utteranceId: input.utteranceId,
    actorId: input.actorId,
    reactionType: input.reactionType,
    intensity: input.intensity,
  });
}
