/**
 * discutere-monitor 状態カード.
 *
 * Discord 接続時、「状態」カテゴリに discutere-monitor チャンネルを冪等生成し、
 * 進行中セッション数 (sessions.ended_at IS NULL) と未処理仮説数
 * (hypotheses.status = 'proposed') と最終更新時間を 1 枚の embed カードとして
 * 定期更新する。Di はカテゴリ管理機構を持たないため、本モジュールが最小レイアウト
 * 生成を内包する (Concordia の ensureDiscordLayout に相当)。
 */

import {
  ChannelType,
  EmbedBuilder,
  type Client,
  type Guild,
  type TextChannel,
} from "discord.js";
import { createCore } from "../core/index.js";

const STATUS_CATEGORY_NAME = "状態";
const MONITOR_CHANNEL_NAME = "discutere-monitor";
const UPDATE_INTERVAL_MS = 60_000;

export interface MonitorCardDeps {
  client: Client;
  /** 集計対象 workspace (command-router と同じ匿名 workspace) */
  workspaceId: string;
  /** discutere-monitor 状態カードの設置先ギルド */
  guildId: string;
}

export class MonitorCard {
  private readonly deps: MonitorCardDeps;
  private timer: NodeJS.Timeout | null = null;
  private messageId: string | null = null;

  constructor(deps: MonitorCardDeps) {
    this.deps = deps;
  }

  start(intervalMs = UPDATE_INTERVAL_MS): void {
    if (this.timer) return;
    void this.tick();
    this.timer = setInterval(() => void this.tick(), intervalMs);
  }

  stop(): void {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }

  // 集計は core の SQLite (KuzuClient.raw) を read-only に都度開いて行う。
  // command-router と同じく createCore() で取得し、使い終わったら close する。
  private counts(): { activeSessions: number; pendingHypotheses: number } {
    const core = createCore();
    try {
      const s = core.client.raw
        .prepare("SELECT COUNT(*) AS n FROM sessions WHERE workspace_id = ? AND ended_at IS NULL")
        .get(this.deps.workspaceId) as { n: number } | undefined;
      const h = core.client.raw
        .prepare("SELECT COUNT(*) AS n FROM hypotheses WHERE workspace_id = ? AND status = 'proposed'")
        .get(this.deps.workspaceId) as { n: number } | undefined;
      return { activeSessions: s?.n ?? 0, pendingHypotheses: h?.n ?? 0 };
    } finally {
      core.close();
    }
  }

  private async ensureMonitorChannel(guild: Guild): Promise<TextChannel | null> {
    await guild.channels.fetch().catch(() => null);
    let category = guild.channels.cache.find(
      (c) => c.type === ChannelType.GuildCategory && c.name === STATUS_CATEGORY_NAME,
    );
    if (!category) {
      category = await guild.channels.create({
        name: STATUS_CATEGORY_NAME,
        type: ChannelType.GuildCategory,
      });
    }
    const existing = guild.channels.cache.find(
      (c) => c.name === MONITOR_CHANNEL_NAME && c.parentId === category!.id,
    );
    const channel =
      existing ??
      (await guild.channels.create({
        name: MONITOR_CHANNEL_NAME,
        type: ChannelType.GuildText,
        parent: category.id,
      }));
    return channel.type === ChannelType.GuildText ? (channel as TextChannel) : null;
  }

  private buildEmbed(activeSessions: number, pendingHypotheses: number): EmbedBuilder {
    const active = activeSessions > 0;
    return new EmbedBuilder()
      .setTitle("🟢 Discutere Monitor")
      .setColor(active ? 0x57f287 : 0x99aab5)
      .addFields(
        { name: "状態", value: active ? "アクティブ" : "アイドル", inline: true },
        { name: "進行中セッション数", value: String(activeSessions), inline: true },
        { name: "未処理仮説数", value: String(pendingHypotheses), inline: true },
      )
      .setFooter({ text: "最終更新" })
      .setTimestamp(new Date());
  }

  private async tick(): Promise<void> {
    try {
      const guild = await this.deps.client.guilds.fetch(this.deps.guildId).catch(() => null);
      if (!guild) return;
      const channel = await this.ensureMonitorChannel(guild);
      if (!channel) return;
      const { activeSessions, pendingHypotheses } = this.counts();
      const embed = this.buildEmbed(activeSessions, pendingHypotheses);

      // プロセス再起動後の重複投稿を避けるため、未キャッシュ時は自分の既存メッセージを採用.
      if (!this.messageId) {
        const recent = await channel.messages.fetch({ limit: 10 }).catch(() => null);
        const mine = recent?.find((m) => m.author.id === this.deps.client.user?.id);
        if (mine) this.messageId = mine.id;
      }
      if (this.messageId) {
        const msg = await channel.messages.fetch(this.messageId).catch(() => null);
        if (msg) {
          await msg.edit({ embeds: [embed] });
          return;
        }
      }
      const sent = await channel.send({ embeds: [embed] });
      this.messageId = sent.id;
    } catch (err) {
      console.warn(`  discord-monitor: tick failed: ${(err as Error).message}`);
    }
  }
}
