/**
 * Di が運用上 自動作成する専用チャンネル (フォーラム集約)。
 *
 * 起動時 (ClientReady) に guild ごとに次を ensure する:
 *   - **データ学習依頼**: 貼られた URL を外部データクロールに回す入口。作成した id は
 *     runtime の crawl 集合に足して既存 crawl-handler が処理する。
 *   - **まとめ投稿**: 収束した議論の結論まとめの集約先 (finalizeForumPost が転記)。
 *
 * 実体の ensure / 作成は system-channel.ts に委譲する (Manage Channels 権限が無ければ
 * 既存同名を流用、無ければ skip)。
 */

import type { Client } from "discord.js";

import { ensureSystemChannel } from "./system-channel.js";

export interface ManagedChannelNames {
  /** データ学習依頼チャンネル名 (既定「データ学習依頼」) */
  dataLearning: string;
  /** まとめ投稿チャンネル名 (既定「まとめ投稿」) */
  summary: string;
  /** 親カテゴリ名 (既定「システム」) */
  category: string;
}

export const DEFAULT_MANAGED_CHANNEL_NAMES: ManagedChannelNames = {
  dataLearning: "データ学習依頼",
  summary: "まとめ投稿",
  category: "システム",
};

export interface EnsureManagedChannelsResult {
  /** クロール入口として監視に足すべきチャンネル id 群 (データ学習依頼)。 */
  dataLearningChannelIds: string[];
}

/**
 * 全 guild に データ学習依頼 / まとめ投稿 を ensure する。
 * データ学習依頼チャンネルの id を返す (caller が crawl 集合へ追加)。
 */
export async function ensureManagedChannels(
  client: Client,
  guildIds: string[],
  names: ManagedChannelNames = DEFAULT_MANAGED_CHANNEL_NAMES
): Promise<EnsureManagedChannelsResult> {
  const dataLearningChannelIds: string[] = [];
  for (const guildId of guildIds) {
    const dataLearning = await ensureSystemChannel(client, {
      guildId,
      categoryName: names.category,
      channelName: names.dataLearning,
      topic: "学習させたいデータの URL を貼ってください (Discutere クロール依頼)",
    });
    if (dataLearning) {
      dataLearningChannelIds.push(dataLearning.id);
      console.log(`  managed-channel: データ学習依頼 = #${dataLearning.name} (${dataLearning.id})`);
    }

    // まとめ投稿は存在保証だけ (転記時に finalizeForumPost が再 ensure する)。
    const summary = await ensureSystemChannel(client, {
      guildId,
      categoryName: names.category,
      channelName: names.summary,
      topic: "収束した議論の結論まとめ (Discutere フォーラム議論)",
    });
    if (summary) {
      console.log(`  managed-channel: まとめ投稿 = #${summary.name} (${summary.id})`);
    }
  }
  return { dataLearningChannelIds };
}
