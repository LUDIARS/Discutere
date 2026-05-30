/**
 * Discord Gateway (WebSocket) transport — WS 型再設計.
 *
 * 旧 HTTP Interactions Endpoint (公開 URL + Ed25519 署名検証) を撤去し、
 * bot token で Gateway に常時接続して push でイベントを受ける。
 *   - 公開 URL 不要 (bot からアウトバウンド接続)
 *   - 接続自体が bot token 認証 → リクエスト毎の署名検証が不要
 *
 * 姉妹サービス Concordia (`src/discord/bot.ts`) と同じ discord.js Client を使い、
 * heartbeat / resume / reconnect を自前実装しない。
 *
 * ドメインロジックは transport 非依存の command-router に委譲する (本ファイルは
 * discord.js 形 ⇄ InboundSlashCommand / DiscordInboundMessage の変換に徹する)。
 */

import {
  Client,
  Events,
  GatewayIntentBits,
  MessageFlags,
  Partials,
  type Interaction,
  type Message,
} from "discord.js";

import { normalizeDiscordInboundMessage } from "./normalize.js";
import {
  routeInboundMessage,
  routeSlashCommand,
  type CommandRouterDeps,
  type InboundSlashCommand,
} from "./command-router.js";
import { MonitorCard } from "./monitor-card.js";

export interface DiscordGatewayDeps extends CommandRouterDeps {
  /** Gateway 接続用 bot token */
  botToken: string;
  /** 単一ギルド運用時の guild id (discutere-monitor 状態カードの設置先) */
  guildId?: string;
}

export interface DiscordGatewayHandle {
  stop(): Promise<void>;
}

/**
 * Gateway を起動する。botToken が空なら null を返して skip (= HTTP health 等は従来通り)。
 */
export async function startDiscordGateway(
  deps: DiscordGatewayDeps
): Promise<DiscordGatewayHandle | null> {
  if (!deps.botToken) {
    console.log("  discord-gateway: skipped (set discord.botToken to enable)");
    return null;
  }

  const client = new Client({
    intents: [
      GatewayIntentBits.Guilds,
      GatewayIntentBits.GuildMessages,
      GatewayIntentBits.MessageContent,
      GatewayIntentBits.DirectMessages,
    ],
    partials: [Partials.Channel, Partials.Message],
  });

  let monitor: MonitorCard | null = null;

  client.once(Events.ClientReady, (c) => {
    console.log(`  discord-gateway: logged in as ${c.user.tag}`);
    if (deps.guildId) {
      monitor = new MonitorCard({
        client,
        workspaceId: deps.workspaceId,
        guildId: deps.guildId,
      });
      monitor.start();
    } else {
      console.log("  discord-monitor: skipped (set discord.guildId to enable)");
    }
  });

  client.on(Events.MessageCreate, (msg: Message) => {
    if (msg.author?.bot) return;
    const normalized = normalizeDiscordInboundMessage({
      id: msg.id,
      channel_id: msg.channelId,
      content: msg.content,
      timestamp: msg.createdAt.toISOString(),
      author: {
        id: msg.author.id,
        username: msg.author.username,
        bot: msg.author.bot,
      },
      mentions: msg.mentions.users.map((u) => ({ id: u.id, username: u.username })),
    });
    if (!normalized) return;
    try {
      routeInboundMessage(normalized, msg.guildId ?? "dm", deps);
    } catch (err) {
      console.warn(`  discord-gateway: message route failed: ${(err as Error).message}`);
    }
  });

  client.on(Events.InteractionCreate, async (interaction: Interaction) => {
    if (!interaction.isChatInputCommand()) return;
    const cmd = toInboundSlashCommand(interaction);
    try {
      const reply = routeSlashCommand(cmd, deps);
      await interaction.reply({
        content: reply.content,
        flags: reply.ephemeral ? MessageFlags.Ephemeral : undefined,
      });
    } catch (err) {
      console.warn(`  discord-gateway: interaction failed id=${interaction.id}: ${(err as Error).message}`);
      if (!interaction.replied && !interaction.deferred) {
        await interaction
          .reply({ content: "⚠️ internal error", flags: MessageFlags.Ephemeral })
          .catch(() => {});
      }
    }
  });

  client.on(Events.Error, (e) => console.warn(`  discord-gateway error: ${e.message}`));

  await client.login(deps.botToken);

  return {
    async stop() {
      monitor?.stop();
      try {
        await client.destroy();
      } catch {
        /* ignore */
      }
    },
  };
}

function toInboundSlashCommand(
  interaction: import("discord.js").ChatInputCommandInteraction
): InboundSlashCommand {
  const argsText = interaction.options.data
    .map((o) => stringifyOptionValue(o.value))
    .filter((s) => s.length > 0)
    .join(" ");
  let enabledOption: boolean | undefined;
  try {
    enabledOption = interaction.options.getBoolean("enabled") ?? undefined;
  } catch {
    enabledOption = undefined;
  }
  return {
    name: interaction.commandName,
    argsText,
    enabledOption,
    userId: interaction.user?.id ?? null,
    guildId: interaction.guildId ?? "dm",
    channelId: interaction.channelId ?? "default",
  };
}

function stringifyOptionValue(value: unknown): string {
  if (typeof value === "string") return value;
  if (typeof value === "number" || typeof value === "boolean") return String(value);
  if (value === undefined || value === null) return "";
  return JSON.stringify(value);
}
