import { parseCommand } from "./command-parser.js";
import { inferRespondsTo } from "./responds-to-inference.js";
import { handleDefine, handleIntends, handleRefine, handleCompare, handleHistory } from "./command-handlers/learning/index.js";
import { handleScene, handleDone } from "./command-handlers/emotion/index.js";
import { handleRefs, handlePropose, handleAddresses, handleJump, handleSubmit, handleValidate, handleIntegrate } from "./command-handlers/synthesis/index.js";
import { handleCrossStub } from "./command-handlers/cross/index.js";
import type { ReturnTypeCreateCore } from "./types.js";

export interface SubmitMessageInput {
  core: ReturnTypeCreateCore;
  workspaceId: string;
  sessionId: string;
  personId?: string;
  rawContent: string;
}

export function submitMessage(input: SubmitMessageInput): { utteranceId: string; commandResult?: { ok: boolean; message?: string; error?: string } } {
  const { core, workspaceId, sessionId, personId, rawContent } = input;
  const parsed = parseCommand(rawContent);
  if (parsed) {
    const ctx = { core, workspaceId, sessionId, personId, args: parsed.args };
    const c = parsed.command;
    const r =
      c === "define" ? handleDefine(ctx) :
      c === "intends" ? handleIntends(ctx) :
      c === "refine" ? handleRefine(ctx) :
      c === "compare" ? handleCompare(ctx) :
      c === "history" ? handleHistory(ctx) :
      c === "scene" ? handleScene(ctx) :
      c === "done" ? handleDone(ctx) :
      c === "refs" ? handleRefs(ctx) :
      c === "propose" ? handlePropose(ctx) :
      c === "addresses" ? handleAddresses(ctx) :
      c === "jump" ? handleJump(ctx) :
      c === "submit" ? handleSubmit(ctx) :
      c === "validate" ? handleValidate(ctx) :
      c === "integrate" ? handleIntegrate(ctx) :
      ["me", "find", "lineage", "cluster", "stale", "hotspot"].includes(c) ? handleCrossStub(ctx, c) :
      { ok: false as const, error: `unknown command: ${c}` };

    if (!r.ok) return { utteranceId: "", commandResult: { ok: false, error: r.error } };
    return { utteranceId: "", commandResult: { ok: true, message: r.message } };
  }

  const respondsTo = inferRespondsTo(rawContent) ?? undefined;
  const utteranceId = core.repos.utterance.create({
    workspaceId,
    sessionId,
    speakerId: personId,
    rawContent,
    postedAt: Date.now(),
    respondsTo,
  } as any);

  return { utteranceId };
}
