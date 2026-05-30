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
import { registerSlashCommands } from "./register-commands.js";

export interface DiscordGatewayDeps extends CommandRouterDeps {
  /** Gateway 接続用 bot token */
  botToken: string;
  /** slash command 登録用 application id (未設定なら client.application.id を使う) */
  applicationId?: string;
  /**
   * 運用 guild id 群 (複数サーバ対応)。
   * - 各 guild に slash command を即時登録 (空なら global 登録)
   * - guild ごとに discutere-monitor 状態カードを設置
   */
  guildIds?: string[];
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

  const monitors: MonitorCard[] = [];
  const guildIds = (deps.guildIds ?? []).filter((g) => g && g !== "dm");

  client.once(Events.ClientReady, async (c) => {
    console.log(`  discord-gateway: logged in as ${c.user.tag}`);

    // slash command を Discord に登録 (これが無いとクライアントに候補が出ない)。
    try {
      const appId = deps.applicationId || c.application?.id || c.user.id;
      const r = await registerSlashCommands({ botToken: deps.botToken, applicationId: appId, guildIds });
      console.log(
        `  discord-gateway: registered ${r.commandCount} slash commands (${r.scope}${
          r.scope === "guild" ? ` x${r.guildIds.length}` : " — 反映に最大1時間"
        })`
      );
    } catch (err) {
      console.warn(`  discord-gateway: slash command 登録失敗: ${(err as Error).message}`);
    }

    // guild ごとに monitor 状態カードを設置 (複数サーバ対応)。
    if (guildIds.length > 0) {
      for (const guildId of guildIds) {
        const card = new MonitorCard({ client, workspaceId: deps.workspaceId, guildId });
        card.start();
        monitors.push(card);
      }
    } else {
      console.log("  discord-monitor: skipped (set discord.guildIds to enable)");
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
    // スレッド内発言は親チャンネルで許可判定する (自然な議論の継承)。
    const parentChannelId = msg.channel?.isThread?.() ? msg.channel.parentId ?? undefined : undefined;
    try {
      const ingested = routeInboundMessage(normalized, msg.guildId ?? "dm", deps, parentChannelId);
      // 取り込んだら 👀 を付けて「議論に乗った」ことを自然にフィードバックする。
      if (ingested) void msg.react("👀").catch(() => {});
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
      for (const m of monitors) m.stop();
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
