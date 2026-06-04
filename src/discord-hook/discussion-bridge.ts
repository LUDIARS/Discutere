/**
 * 議論 utterance / hypothesis を Discord channel に投稿するブリッジ (PR-I).
 *
 * 流れ:
 *   1. persona-engine が postUtterance / proposeHypothesis を呼ぶ
 *   2. discatier-engine-adapter の onAfterPost callback で本ブリッジが発火
 *   3. session.scene から "discord:<guildId>/<channelId>" を取り出し
 *   4. workspace 配下の discord monitor の botToken で channel に投稿
 *
 * 失敗は warn のみ (= 議論を止めない)。
 */

import { getConfig } from "../config.js";
import type { createCore } from "../core/index.js";
import { monitorRepo } from "../db/repository.js";
import {
  ensureChannelWebhook,
  humanizeForDiscord,
  postDiscordChannel,
  postDiscordWebhook,
} from "./poster.js";

type Core = ReturnType<typeof createCore>;

export interface DiscussionPostArgs {
  core: Core;
  workspaceId: string;
  sessionId: string;
  /** "utterance" | "hypothesis" */
  kind: "utterance" | "hypothesis";
  /** 発話者 (persona name 等、 表示用) */
  speakerLabel: string;
  text: string;
  /**
   * true: persona を webhook で人間名 (speakerLabel) として投稿する。
   * false/未指定: Discutere bot として直接投稿する (= 進行役)。
   */
  viaWebhook?: boolean;
}

export async function postDiscussionToDiscord(args: DiscussionPostArgs): Promise<{
  ok: boolean;
  reason?: string;
  channelId?: string;
}> {
  // session.scene = "discord:<guildId>/<channelId>" or "gap:<gapId>" (gap session 用)
  const session = args.core.client.raw
    .prepare("SELECT scene FROM sessions WHERE id = ?")
    .get(args.sessionId) as { scene: string | null } | undefined;
  if (!session?.scene || !session.scene.startsWith("discord:")) {
    return { ok: false, reason: "session.scene is not discord-bound" };
  }
  const guildChannel = session.scene.slice("discord:".length);
  const [guildId, channelId] = guildChannel.split("/");
  if (!channelId) {
    return { ok: false, reason: "session.scene channel parse failed" };
  }

  const monitors = await monitorRepo.findByWorkspaceId(args.workspaceId);
  const discordMonitor = monitors.find(
    (m) => m.platform === "discord" && m.botToken && (!guildId || m.botWorkspaceId === guildId)
  );
  // Discord-only pivot: channel_monitors に登録が無くても、 Gateway 接続と同じ
  // config の bot token で投稿する (bot は既に当該 guild に在席している)。
  const botToken = discordMonitor?.botToken ?? getConfig().discord.botToken;
  if (!botToken) {
    return { ok: false, reason: "no discord bot token (channel_monitors / config どちらも未設定)" };
  }

  const body = humanizeForDiscord(args.text);

  // persona は webhook で人間名として投稿 (議論参加者らしく見せる)。
  if (args.viaWebhook) {
    try {
      const wh = await ensureChannelWebhook(botToken, channelId);
      await postDiscordWebhook({
        webhookId: wh.id,
        webhookToken: wh.token,
        username: args.speakerLabel,
        content: body,
      });
      return { ok: true, channelId };
    } catch (err) {
      // webhook 権限が無い等で失敗 → bot 直接投稿に fallback (名前を本文先頭に)
      try {
        await postDiscordChannel({ botToken, channelId, content: `**${args.speakerLabel}**\n${body}` });
        return { ok: true, channelId };
      } catch (err2) {
        return { ok: false, reason: `webhook(${(err as Error).message}) / bot(${(err2 as Error).message})` };
      }
    }
  }

  // 進行役 (Discutere) は bot として直接投稿。
  try {
    await postDiscordChannel({ botToken, channelId, content: body });
    return { ok: true, channelId };
  } catch (err) {
    return { ok: false, reason: (err as Error).message };
  }
}
