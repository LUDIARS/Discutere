/**
 * Discord Interactions parser + verifier (PR-B).
 *
 * - Ed25519 検証は ./signature-verify.ts の verifyDiscordSignature を使う
 * - APPLICATION_COMMAND (type=2) → Discatier の "/<name> <args>" 文字列に変換
 *   (= command-parser.ts と互換、 message-input.submitMessage 経路に流せる)
 *
 * Phase 0 では PING (type=1) と APPLICATION_COMMAND (type=2) のみ対応。
 * MESSAGE_COMPONENT (type=3) や AUTOCOMPLETE (type=4) は別 PR で。
 */

import { verifyDiscordSignature } from "./signature-verify.js";

export const INTERACTION_TYPE = {
  PING: 1,
  APPLICATION_COMMAND: 2,
  MESSAGE_COMPONENT: 3,
  APPLICATION_COMMAND_AUTOCOMPLETE: 4,
  MODAL_SUBMIT: 5,
} as const;

export const INTERACTION_RESPONSE_TYPE = {
  PONG: 1,
  CHANNEL_MESSAGE_WITH_SOURCE: 4,
  DEFERRED_CHANNEL_MESSAGE_WITH_SOURCE: 5,
} as const;

export interface DiscordInteractionOption {
  name: string;
  type: number;
  value: unknown;
}

export interface DiscordInteraction {
  id: string;
  application_id: string;
  type: number;
  data?: {
    id?: string;
    name?: string;
    options?: DiscordInteractionOption[];
  };
  guild_id?: string;
  channel_id?: string;
  member?: { user?: { id: string; username?: string } };
  user?: { id: string; username?: string };
  token: string;
  version: number;
}

export interface VerifyAndParseArgs {
  rawBody: string;
  signature: string;
  timestamp: string;
  publicKey: string;
}

export type VerifyAndParseResult =
  | { ok: true; interaction: DiscordInteraction }
  | { ok: false; status: 401 | 400; error: string };

export function verifyAndParseInteraction(
  args: VerifyAndParseArgs
): VerifyAndParseResult {
  if (
    !verifyDiscordSignature({
      rawBody: args.rawBody,
      signature: args.signature,
      timestamp: args.timestamp,
      publicKey: args.publicKey,
    })
  ) {
    return { ok: false, status: 401, error: "invalid signature" };
  }
  try {
    const parsed = JSON.parse(args.rawBody) as DiscordInteraction;
    if (typeof parsed.type !== "number") {
      return { ok: false, status: 400, error: "interaction.type missing" };
    }
    return { ok: true, interaction: parsed };
  } catch {
    return { ok: false, status: 400, error: "invalid json" };
  }
}

/**
 * APPLICATION_COMMAND を Discatier の `/command args...` 文字列に変換。
 * type 以外の interaction は null。
 */
export function interactionToCommand(
  interaction: DiscordInteraction
): string | null {
  if (interaction.type !== INTERACTION_TYPE.APPLICATION_COMMAND) return null;
  const name = interaction.data?.name;
  if (!name) return null;
  const args = (interaction.data?.options ?? [])
    .map((o) => {
      if (typeof o.value === "string") return o.value;
      if (typeof o.value === "number" || typeof o.value === "boolean") return String(o.value);
      return JSON.stringify(o.value);
    })
    .filter((s) => s.length > 0)
    .join(" ");
  return args.length > 0 ? `/${name} ${args}` : `/${name}`;
}

/** interaction の発話者 id (DM ならば user.id、 guild なら member.user.id) */
export function getInteractionSpeakerId(
  interaction: DiscordInteraction
): string | null {
  return interaction.member?.user?.id ?? interaction.user?.id ?? null;
}

/** PING type の interaction か */
export function isPing(interaction: DiscordInteraction): boolean {
  return interaction.type === INTERACTION_TYPE.PING;
}

/** PONG レスポンスのボディ (Discord 仕様で type: 1) */
export function pongResponse(): { type: number } {
  return { type: INTERACTION_RESPONSE_TYPE.PONG };
}

/** 一般 reply (channel message) のボディ */
export function channelMessageResponse(
  content: string,
  options: { ephemeral?: boolean } = {}
): { type: number; data: { content: string; flags?: number } } {
  return {
    type: INTERACTION_RESPONSE_TYPE.CHANNEL_MESSAGE_WITH_SOURCE,
    data: {
      content,
      // ephemeral = flags 64 (= 1 << 6, EPHEMERAL)
      ...(options.ephemeral ? { flags: 64 } : {}),
    },
  };
}
