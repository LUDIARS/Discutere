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
import { handleCrawlMessage } from "./crawl-handler.js";
import type { CrawlDeps } from "./crawl-channel.js";
import {
  finalizeForumPost,
  handleForumReply,
  handleForumThreadCreate,
  isForumStarterMessage,
  isForumThreadChannel,
} from "./forum-monitor.js";
import { ensureManagedChannels } from "./managed-channels.js";
import type { AnyThreadChannel } from "discord.js";
import { createDebateRunner, type DebateRunner } from "../discussion/director-live.js";

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
  /** メッセージへのリアクション付与イベント (議論意見スコアリング用)。 */
  onReaction?: (info: { messageId: string; channelId: string; emoji: string; userId: string }) => void;
  /**
   * データクロール用チャンネル id。 ここに貼られた URL は議論取り込みではなく
   * 外部議論データのクロール (crawl-handler) に回す。 空ならクロール無効。
   */
  crawlChannelIds?: string[];
  /** クロール取り込みの依存 (createCore / workspaceId / youtubeApiKey)。 未設定ならクロール skip。 */
  crawlDeps?: CrawlDeps;
  /**
   * フォーラム集約設定。enabled なら guild 内の全 Forum チャンネルを監視し、
   * 起動時に データ学習依頼 / まとめ投稿 チャンネルを自動作成する。
   */
  forum?: {
    enabled: boolean;
    summaryChannelName: string;
    dataLearningChannelName: string;
    managedCategoryName: string;
    improvementTagNames: string[];
    funTagNames: string[];
  };
  /** /debate のパーティ議論設定 (config.discussion)。未設定なら /debate 無効。 */
  debate?: import("../config.js").DiscutereConfig["discussion"];
  /** claude -p 用 git-bash パス (Windows)。 */
  gitBashPath?: string;
}

export interface DiscordGatewayHandle {
  stop(): Promise<void>;
  /**
   * 収束したフォーラムポストを締める (archive+lock + まとめ転記)。
   * scene がフォーラムスレッドでなければ no-op。facilitator の onConverged から呼ぶ。
   */
  finalizeForumPost(args: {
    scene: string | null;
    summary: string;
    title: string;
  }): Promise<{ closed: boolean; reason?: string }>;
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
      GatewayIntentBits.GuildMessageReactions,
    ],
    partials: [Partials.Channel, Partials.Message, Partials.Reaction],
  });

  // /debate のパーティ議論ランナー (config.discussion があれば有効)。
  const debateRunner: DebateRunner | null = deps.debate
    ? createDebateRunner({
        client,
        botToken: deps.botToken,
        discussion: deps.debate,
        gitBashPath: deps.gitBashPath,
      })
    : null;

  const monitors: MonitorCard[] = [];
  const guildIds = (deps.guildIds ?? []).filter((g) => g && g !== "dm");
  // crawl 対象は config 由来 + 起動時に自動作成する「データ学習依頼」チャンネルを足す (mutable)。
  const crawlChannelIds = new Set((deps.crawlChannelIds ?? []).filter(Boolean));
  const forumEnabled = deps.forum?.enabled ?? false;

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

    // フォーラム集約: データ学習依頼 / まとめ投稿 を自動作成し、前者を crawl 監視に足す。
    if (forumEnabled && deps.forum && guildIds.length > 0) {
      try {
        const { dataLearningChannelIds } = await ensureManagedChannels(client, guildIds, {
          dataLearning: deps.forum.dataLearningChannelName,
          summary: deps.forum.summaryChannelName,
          category: deps.forum.managedCategoryName,
        });
        for (const id of dataLearningChannelIds) crawlChannelIds.add(id);
        console.log(`  discord-forum: monitoring all guild forums; managed channels ensured`);
      } catch (err) {
        console.warn(`  discord-forum: managed channel 作成失敗: ${(err as Error).message}`);
      }
    }
  });

  // フォーラム新規ポスト (親=GuildForum) の最初の投稿で議論を起こす。
  if (forumEnabled) {
    client.on(Events.ThreadCreate, (thread: AnyThreadChannel, newlyCreated: boolean) => {
      if (!newlyCreated) return;
      void handleForumThreadCreate(thread, {
        router: deps,
        directionConfig: deps.forum
          ? { improvementTagNames: deps.forum.improvementTagNames, funTagNames: deps.forum.funTagNames }
          : undefined,
      }).catch((err) =>
        console.warn(`  discord-forum: thread-create 失敗: ${(err as Error).message}`)
      );
    });
  }

  client.on(Events.MessageCreate, (msg: Message) => {
    if (msg.author?.bot) return;

    // フォーラムスレッド内の投稿: starter は ThreadCreate が処理済 → ここでは返信のみ取り込む。
    if (forumEnabled && isForumThreadChannel(msg.channel)) {
      if (!isForumStarterMessage(msg)) handleForumReply(msg, { router: deps });
      return;
    }

    // データクロール用チャンネル: 貼られた URL を外部データ取り込みに回す (議論取り込みはしない)。
    const parentForCrawl = msg.channel?.isThread?.() ? msg.channel.parentId ?? undefined : undefined;
    if (
      deps.crawlDeps &&
      (crawlChannelIds.has(msg.channelId) || (parentForCrawl && crawlChannelIds.has(parentForCrawl)))
    ) {
      void handleCrawlMessage(client, msg, { crawl: deps.crawlDeps }).catch((err) =>
        console.warn(`  discord-gateway: crawl 失敗: ${(err as Error).message}`)
      );
      return;
    }

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
      const { ingested, seed } = routeInboundMessage(
        normalized,
        msg.guildId ?? "dm",
        deps,
        parentChannelId
      );
      // 取り込んだ全件ではなく「議論の種(開始エントリ)になった投稿」にだけ 👀 を付ける。
      // こうするとリアクション=議論が立った合図になり、persona-engine の返信と対応する。
      if (ingested && seed) {
        void seed
          .then((started) => {
            if (started) void msg.react("👀").catch(() => {});
          })
          .catch(() => {});
      }
    } catch (err) {
      console.warn(`  discord-gateway: message route failed: ${(err as Error).message}`);
    }
  });

  // 議論意見へのリアクション → スコアリング (deps.onReaction が処理)。
  client.on(Events.MessageReactionAdd, (reaction, user) => {
    try {
      if (user?.bot) return;
      const emoji = reaction.emoji?.name ?? reaction.emoji?.toString() ?? "";
      const messageId = reaction.message?.id;
      const channelId = reaction.message?.channelId ?? "";
      if (!messageId || !emoji) return;
      deps.onReaction?.({ messageId, channelId, emoji, userId: user?.id ?? "" });
    } catch (err) {
      console.warn(`  discord-gateway: reaction handle failed: ${(err as Error).message}`);
    }
  });

  client.on(Events.InteractionCreate, async (interaction: Interaction) => {
    // 続行/停止ボタン (debate:cont/stop:<token>)。
    if (interaction.isButton()) {
      if (debateRunner) await debateRunner.handleButton(interaction).catch(() => {});
      return;
    }
    if (!interaction.isChatInputCommand()) return;

    // /debate: パーティ議論を開始 (非同期、ack だけ即返す)。
    if (interaction.commandName === "debate") {
      if (!debateRunner) {
        await interaction.reply({ content: "議論機能が無効です (config.discussion 未設定)", flags: MessageFlags.Ephemeral }).catch(() => {});
        return;
      }
      const topic = interaction.options.getString("topic", true);
      await interaction.reply({ content: `🗣️ 議論を開始します: 「${topic}」`, flags: MessageFlags.Ephemeral }).catch(() => {});
      void debateRunner.start(interaction.channelId, topic);
      return;
    }

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
    finalizeForumPost(args) {
      if (!forumEnabled) return Promise.resolve({ closed: false, reason: "forum disabled" });
      return finalizeForumPost(client, {
        scene: args.scene,
        summary: args.summary,
        title: args.title,
        summaryChannelName: deps.forum?.summaryChannelName,
        categoryName: deps.forum?.managedCategoryName,
      });
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
