// @implements DI-EXTERNAL-DISCUSSION
export interface ConversationMessage {
  id: string;
  kind: "human" | "ai";
  text: string;
  at: number;
}
export interface ConversationInput {
  scene: string;
  enabled: boolean;
  messages: ConversationMessage[];
}
export interface ParticipationResult {
  status: "disabled" | "waiting" | "busy" | "skipped" | "proposal";
  proposalId?: string;
  text?: string;
  stance?: "pro" | "con";
  sourceMessageId?: string;
}
export function parseConversation(value: unknown): ConversationInput {
  if (!value || typeof value !== "object") throw new Error("conversation required");
  const input = value as Record<string, unknown>;
  if (typeof input.scene !== "string" || !/^actio:[a-zA-Z0-9:_-]{1,220}$/.test(input.scene)) throw new Error("invalid scene");
  if (typeof input.enabled !== "boolean" || !Array.isArray(input.messages) || input.messages.length > 80) throw new Error("invalid conversation");
  const seen = new Set<string>();
  let bytes = 0;
  const messages = input.messages.map((item: unknown): ConversationMessage => {
    if (!item || typeof item !== "object") throw new Error("invalid message");
    const m = item as Record<string, unknown>;
    if (typeof m.id !== "string" || !m.id || m.id.length > 200 || seen.has(m.id)
      || (m.kind !== "human" && m.kind !== "ai") || typeof m.text !== "string" || m.text.length > 8000
      || typeof m.at !== "number" || !Number.isSafeInteger(m.at) || m.at < 0) throw new Error("invalid message");
    seen.add(m.id); bytes += m.text.length;
    return { id: m.id, kind: m.kind, text: m.text, at: m.at };
  });
  if (bytes > 32000) throw new Error("conversation too large");
  messages.sort((a, b) => a.at - b.at || a.id.localeCompare(b.id));
  return { scene: input.scene, enabled: input.enabled, messages };
}
