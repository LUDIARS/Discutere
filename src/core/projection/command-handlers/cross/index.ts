import type { HandlerContext, HandlerResult } from "../types.js";

export function handleCrossStub(_ctx: HandlerContext, command: string): HandlerResult {
  return { ok: true, message: `${command} is stub (implemented in phase 6)` };
}
