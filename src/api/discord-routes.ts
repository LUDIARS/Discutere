/**
 * Discord Interactions endpoint (PR-B).
 *
 * - POST /api/discord/interactions
 *   * Ed25519 検証 (workspace 配下 discord monitor の public key いずれかで一致)
 *   * PING (type=1) は PONG 返却 (Discord console の登録 handshake)
 *   * APPLICATION_COMMAND (type=2) は Discatier の command 文字列に変換し、
 *     submitMessage(workspaceId, sessionId, "/<name> <args>") で flow 流し込み
 *
 * sessionId 解決:
 *   1. interaction.guild_id + channel_id から既存 session を引く
 *   2. なければ "discord-session:<guildId>:<channelId>" で auto-create
 *
 * workspaceId は ?workspaceId=<id> クエリで明示指定 (= 同 guild が複数 workspace に
 * 紐付くケースを許す)。 未指定なら 400。
 */

import { Hono } from "hono";

import { monitorRepo } from "../db/repository.js";
import {
  channelMessageResponse,
  interactionToCommand,
  isPing,
  pongResponse,
  verifyAndParseInteraction,
} from "../discord-hook/interactions.js";
import { createCore } from "../core/index.js";
import { submitMessage } from "../core/projection/message-input.js";

export const discordRoutes = new Hono();

discordRoutes.post("/discord/interactions", async (c) => {
  const rawBody = await c.req.text();
  const workspaceId = c.req.query("workspaceId");
  if (!workspaceId) {
    return c.json({ error: "workspaceId query parameter required" }, 400);
  }

  const monitors = await monitorRepo.findByWorkspaceId(workspaceId);
  const publicKeys = monitors
    .filter((m) => m.platform === "discord" && !!m.botSigningSecret)
    .map((m) => m.botSigningSecret as string);
  if (publicKeys.length === 0) {
    return c.json({ error: "no discord monitor with public key" }, 401);
  }

  const signature = c.req.header("x-signature-ed25519") ?? "";
  const timestamp = c.req.header("x-signature-timestamp") ?? "";

  const verified = (() => {
    for (const pk of publicKeys) {
      const r = verifyAndParseInteraction({ rawBody, signature, timestamp, publicKey: pk });
      if (r.ok) return r;
    }
    return { ok: false as const, status: 401 as const, error: "invalid signature" };
  })();

  if (!verified.ok) {
    return c.json({ error: verified.error }, verified.status);
  }

  const interaction = verified.interaction;

  // PING handshake
  if (isPing(interaction)) {
    return c.json(pongResponse());
  }

  // APPLICATION_COMMAND → Discatier command 文字列
  const commandText = interactionToCommand(interaction);
  if (!commandText) {
    return c.json(
      channelMessageResponse("unsupported interaction type", { ephemeral: true })
    );
  }

  // session 解決
  const guildId = interaction.guild_id ?? "dm";
  const channelId = interaction.channel_id ?? "default";
  const core = createCore();
  try {
    const sessionId = ensureDiscordSession(core, workspaceId, guildId, channelId);
    const res = submitMessage({
      core,
      workspaceId,
      sessionId,
      rawContent: commandText,
    });
    const result = res.commandResult;
    const message = result
      ? result.ok
        ? `✅ ${result.message ?? "ok"}`
        : `⚠️ ${result.error ?? "command failed"}`
      : "message routed";
    return c.json(channelMessageResponse(message, { ephemeral: !result?.ok }));
  } finally {
    core.close();
  }
});

function ensureDiscordSession(
  core: ReturnType<typeof createCore>,
  workspaceId: string,
  guildId: string,
  channelId: string
): string {
  const title = `discord-session:${guildId}:${channelId}`;
  const existing = core.client.raw
    .prepare(
      "SELECT id FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1"
    )
    .get(workspaceId, title) as { id: string } | undefined;
  if (existing) return existing.id;
  return core.repos.session.create({
    workspaceId,
    title,
    startedAt: Date.now(),
    scene: `discord:${guildId}/${channelId}`,
  } as never);
}
