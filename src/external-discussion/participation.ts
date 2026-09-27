// @implements DI-EXTERNAL-DISCUSSION
import { createHash } from "node:crypto";
import type { ConversationInput } from "./contracts.js";

export const SILENCE_MS = 180_000;
export interface Opportunity { id: string; humanId: string; humanAt: number; mode: "quiet" | "active" }
/** Only new human activity can arm participation. Poll frequency must not affect probability. */
export function opportunity(input: ConversationInput, now: number): Opportunity | null {
  if (!input.enabled) return null;
  const humans = input.messages.filter(m => m.kind === "human" && m.text.trim());
  const last = humans.at(-1);
  if (!last || last.at > now || now - last.at > 86_400_000) return null;
  // A bot response after the last human already ended this opportunity.
  if (input.messages.some(m => m.kind === "ai" && m.at >= last.at)) return null;
  const quiet = now - last.at >= SILENCE_MS;
  if (!quiet && humans.filter(m => m.at >= now - SILENCE_MS).length < 3) return null;
  const mode = quiet ? "quiet" : "active";
  const id = createHash("sha256").update(JSON.stringify([input.scene, last.id, last.text, mode])).digest("hex");
  return { id, humanId: last.id, humanAt: last.at, mode };
}
