/**
 * ゲーム感想チャンネルの ensure / 抽出。
 *
 * - カテゴリ「ゲーム感想」を起動時に ensure (無ければ作成、Manage Channels 権限が要る)。
 *   配下のチャンネル (チャンネル名 = ゲームタイトル) はユーザが作って投稿する。
 * - そのカテゴリ配下のチャンネルへの投稿は、議論にせず感想 (意見データ) として収集する。
 *
 * 注意: Discord はチャンネル名を正規化する (小文字・ハイフン化)。gameTitle は
 * チャンネル名そのまま (= 正規化後の slug) を採用する。
 */

import { ChannelType, type Client, type Message } from "discord.js";

/** 各 guild に感想カテゴリを ensure (無ければ作成)。作成失敗 (権限不足等) は黙って skip。 */
export async function ensureGameFeedbackCategory(
  client: Client,
  guildIds: string[],
  categoryName: string
): Promise<void> {
  for (const guildId of guildIds) {
    try {
      const guild = await client.guilds.fetch(guildId);
      const existing = guild.channels.cache.find(
        (c) => c.type === ChannelType.GuildCategory && c.name === categoryName
      );
      if (existing) {
        console.log(`  game-feedback: category #${categoryName} ok (${existing.id})`);
        continue;
      }
      const created = await guild.channels.create({ name: categoryName, type: ChannelType.GuildCategory });
      console.log(`  game-feedback: created category #${categoryName} (${created.id})`);
    } catch (err) {
      console.warn(`  game-feedback: ensure category failed (${guildId}): ${(err as Error).message}`);
    }
  }
}

export interface ExtractedFeedback {
  gameTitle: string;
  content: string;
  createdAt: number;
}

/**
 * メッセージが感想カテゴリ配下のチャンネルへの投稿なら、感想として抽出する。
 * gameTitle = チャンネル名。投稿者は含めない (匿名)。該当しなければ null。
 */
export function extractGameFeedback(msg: Message, categoryName: string): ExtractedFeedback | null {
  const channel = msg.channel;
  // 通常のギルドテキスト/announcement チャンネルのみ対象 (スレッドやDMは除外)。
  if (channel.type !== ChannelType.GuildText && channel.type !== ChannelType.GuildAnnouncement) return null;
  const parent = channel.parent;
  if (!parent || parent.type !== ChannelType.GuildCategory || parent.name !== categoryName) return null;
  const content = msg.content?.trim() ?? "";
  if (content.length === 0) return null;
  return { gameTitle: channel.name, content, createdAt: msg.createdTimestamp };
}
