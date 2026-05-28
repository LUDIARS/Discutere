import type { DiscordInboundAuthor, DiscordInboundMention, DiscordInboundMessage } from "./types.js";

type UnknownRecord = Record<string, unknown>;

export function normalizeDiscordInboundMessage(payload: unknown): DiscordInboundMessage | null {
  if (!isRecord(payload)) return null;

  // Raw webhook style: { id, channel_id, author, content, mentions, timestamp }
  if (typeof payload.content === "string" && isRecord(payload.author)) {
    return fromRawMessage(payload);
  }

  // Gateway style: { t: "MESSAGE_CREATE", d: {...message...} }
  if (payload.t === "MESSAGE_CREATE" && isRecord(payload.d)) {
    return fromRawMessage(payload.d);
  }

  // Event wrapper style: { event: { ...message... } }
  if (isRecord(payload.event)) {
    return normalizeDiscordInboundMessage(payload.event);
  }

  return null;
}

function fromRawMessage(raw: UnknownRecord): DiscordInboundMessage | null {
  const id = asString(raw.id);
  const channelId = asString(raw.channel_id);
  const content = asString(raw.content);
  const timestamp = asString(raw.timestamp);
  const author = asAuthor(raw.author);

  if (!id || !channelId || !content || !timestamp || !author) return null;
  const mentions = asMentions(raw.mentions);
  return { id, channelId, author, content, mentions, timestamp };
}

function asAuthor(v: unknown): DiscordInboundAuthor | null {
  if (!isRecord(v)) return null;
  const id = asString(v.id);
  const username = asString(v.username);
  if (!id || !username) return null;
  return {
    id,
    username,
    bot: typeof v.bot === "boolean" ? v.bot : undefined,
  };
}

function asMentions(v: unknown): DiscordInboundMention[] {
  if (!Array.isArray(v)) return [];
  const out: DiscordInboundMention[] = [];
  for (const item of v) {
    if (!isRecord(item)) continue;
    out.push({
      id: asString(item.id) ?? undefined,
      username: asString(item.username) ?? undefined,
    });
  }
  return out;
}

function asString(v: unknown): string | null {
  if (typeof v !== "string") return null;
  const s = v.trim();
  return s.length > 0 ? s : null;
}

function isRecord(v: unknown): v is UnknownRecord {
  return typeof v === "object" && v !== null;
}

