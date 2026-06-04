/**
 * crawl チャンネルのメッセージを取り込み処理に流す orchestrator (id=60 + id=61)。
 *
 * gateway (discord.js) から呼ばれる。 メッセージ本文から URL を取り出して取り込み、
 * 進捗をリアクションで示し、 結果を「データ追加」 通知チャンネルへ投稿する。
 * 取り込みロジック自体は crawl-channel.ts (transport 非依存) に委譲する。
 */

import type { Client, Message } from "discord.js";

import {
  formatCrawlSummary,
  ingestCrawlUrls,
  parseCrawlMessage,
  type CrawlDeps,
} from "./crawl-channel.js";
import { ensureSystemChannel } from "./system-channel.js";

export interface CrawlHandlerDeps {
  crawl: CrawlDeps;
  /** 通知先の親カテゴリ名 (既定「システム」) */
  categoryName?: string;
  /** 通知先チャンネル名 (既定「データ追加」) */
  channelName?: string;
}

async function safeReact(msg: Message, emoji: string): Promise<void> {
  try {
    await msg.react(emoji);
  } catch {
    /* リアクション権限が無くても処理は続行 */
  }
}

/**
 * crawl メッセージ 1 件を処理する。 URL が無ければ何もしない (false)。
 * 取り込みを試みたら true。 例外は内部で握り、 ⚠️ を付けて返す。
 */
export async function handleCrawlMessage(
  client: Client,
  msg: Message,
  deps: CrawlHandlerDeps
): Promise<boolean> {
  const { gameSlug, urls } = parseCrawlMessage(msg.content ?? "");
  if (urls.length === 0) return false;

  await safeReact(msg, "⏳");
  let summary: string;
  let ok = false;
  try {
    const results = await ingestCrawlUrls(deps.crawl, { urls, gameSlugHint: gameSlug });
    ok = results.some((r) => r.imported > 0);
    summary = formatCrawlSummary(results);
  } catch (err) {
    summary = `⚠️ クロール失敗: ${(err as Error).message}`;
  }

  await safeReact(msg, ok ? "✅" : "⚠️");

  // 「データ追加」 チャンネルへ通知 (guild 内のみ。 無ければ作成、 権限無ければ元チャンネルに返す)。
  const guildId = msg.guildId;
  let posted = false;
  if (guildId) {
    const channel = await ensureSystemChannel(client, {
      guildId,
      categoryName: deps.categoryName,
      channelName: deps.channelName,
    });
    if (channel) {
      try {
        await channel.send(summary);
        posted = true;
      } catch {
        /* 送信失敗 → 元チャンネルにフォールバック */
      }
    }
  }
  if (!posted) {
    try {
      await msg.reply(summary);
    } catch {
      /* 返信も不可なら諦める (リアクションは付いている) */
    }
  }
  return true;
}
