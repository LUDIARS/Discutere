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
  resolveWebhookTarget,
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
  /**
   * true: 失敗ではなく「意図的に投稿しなかった」(= headless 議論など非 discord scene)。
   * caller はこの場合 warn を出さずに黙ってスキップする (ログ noise 除去 / #63)。
   */
  skipped?: boolean;
  reason?: string;
  channelId?: string;
  messageId?: string;
}> {
  // session.scene = "discord:<guildId>/<channelId>" or "gap:<gapId>" (gap session 用)
  const session = args.core.client.raw
    .prepare("SELECT scene FROM sessions WHERE id = ?")
    .get(args.sessionId) as { scene: string | null } | undefined;
  if (!session?.scene || !session.scene.startsWith("discord:")) {
    // headless (gap:<gapId> 等) の議論は Discord に出さないのが正常動作。
    // 失敗ではないので skipped で返し、 caller の warn を抑止する。
    return { ok: false, skipped: true, reason: "session.scene is not discord-bound (headless)" };
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

  // 全 persona (進行役含む) を webhook で人間名として投稿する (役割名は出さない)。
  if (args.viaWebhook) {
    try {
      // フォーラムスレッドは webhook を持てないので、 親フォーラムの webhook +
      // thread_id でスレッドに流す (通常チャンネルはそのまま)。
      const { webhookChannelId, threadId } = await resolveWebhookTarget(botToken, channelId);
      const wh = await ensureChannelWebhook(botToken, webhookChannelId);
      const r = await postDiscordWebhook({
        webhookId: wh.id,
        webhookToken: wh.token,
        username: args.speakerLabel,
        content: body,
        threadId,
      });
      return { ok: true, channelId, messageId: r.id };
    } catch (err) {
      // webhook 権限が無い等で失敗 → bot 直接投稿に fallback (名前を本文先頭に)
      try {
        const r = await postDiscordChannel({ botToken, channelId, content: `**${args.speakerLabel}**\n${body}` });
        return { ok: true, channelId, messageId: r.id };
      } catch (err2) {
        return { ok: false, reason: `webhook(${(err as Error).message}) / bot(${(err2 as Error).message})` };
      }
    }
  }

  // viaWebhook=false の経路 (名前なし bot 投稿)。 現状は全 persona webhook なので未使用。
  try {
    const r = await postDiscordChannel({ botToken, channelId, content: body });
    return { ok: true, channelId, messageId: r.id };
  } catch (err) {
    return { ok: false, reason: (err as Error).message };
  }
}
