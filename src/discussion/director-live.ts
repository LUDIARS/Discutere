/**
 * DiscussionDirector の live アダプタ (Discord + claude -p)。
 *
 * - LLM       : ClaudeCliClient (claude -p、メンバーの model を --model で渡す)
 * - 発話投稿  : postDiscordWebhook (persona の人間名で投稿、フォーラムスレッド対応)
 * - 続行/停止 : Discord ボタン [続ける][止める] の interaction (無応答はタイムアウト→停止)
 *
 * gateway から createDebateRunner() を 1 つ作り、slash `/debate` で start()、
 * InteractionCreate の button を handleButton() に渡す。
 */

import {
  ActionRowBuilder,
  ButtonBuilder,
  ButtonStyle,
  type ButtonInteraction,
  type Client,
} from "discord.js";

import type { DiscutereConfig } from "../config.js";
import { ClaudeCliClient } from "../persona-engine/llm/claude-cli.js";
import { ensureChannelWebhook, postDiscordWebhook, resolveWebhookTarget } from "../discord-hook/poster.js";
import { runDiscussion, type DirectorDeps } from "./director.js";
import type { PartyConfig } from "./composition.js";

const CUSTOM_PREFIX = "debate";

interface PendingContinue {
  resolve: (cont: boolean) => void;
  timer: ReturnType<typeof setTimeout>;
}

export interface DebateRunner {
  /** チャンネル (or フォーラムスレッド) で議論を開始する。 */
  start(channelId: string, topic: string): Promise<void>;
  /** button interaction を処理する。続行/停止ボタンなら true。 */
  handleButton(interaction: ButtonInteraction): Promise<boolean>;
  /** 進行中の議論数。 */
  activeCount(): number;
}

export function createDebateRunner(opts: {
  client: Client;
  botToken: string;
  discussion: DiscutereConfig["discussion"];
  gitBashPath?: string;
  claudeCliTimeoutMs?: number;
}): DebateRunner {
  const llm = new ClaudeCliClient({
    defaultTimeoutMs: opts.claudeCliTimeoutMs ?? 90_000,
    gitBashPath: opts.gitBashPath,
  });
  const pending = new Map<string, PendingContinue>();
  let tokenSeq = 0;
  let active = 0;

  function partyConfig(): PartyConfig {
    const d = opts.discussion;
    return {
      expectedUtterances: d.expectedUtterances,
      keymanCount: d.keymanCount,
      facilitatorModel: d.facilitatorModel,
      keymanModel: d.keymanModel,
      opinionModels: d.opinionModels,
      minTotal: d.minTotal,
    };
  }

  async function postAs(channelId: string, username: string, content: string): Promise<void> {
    const wh = await ensureChannelWebhook(channelId, opts.botToken);
    const target = await resolveWebhookTarget(channelId, opts.botToken);
    await postDiscordWebhook({
      webhookId: wh.id,
      webhookToken: wh.token,
      username,
      content,
      threadId: target.threadId,
    });
  }

  async function askContinue(channelId: string, ctx: { round: number; total: number }): Promise<boolean> {
    const token = `${Date.now()}-${tokenSeq++}`;
    const row = new ActionRowBuilder<ButtonBuilder>().addComponents(
      new ButtonBuilder().setCustomId(`${CUSTOM_PREFIX}:cont:${token}`).setLabel("続ける").setStyle(ButtonStyle.Success),
      new ButtonBuilder().setCustomId(`${CUSTOM_PREFIX}:stop:${token}`).setLabel("止める").setStyle(ButtonStyle.Danger)
    );
    const channel = await opts.client.channels.fetch(channelId);
    if (!channel || !channel.isTextBased() || !("send" in channel)) return false;
    await (channel as { send: (o: unknown) => Promise<unknown> }).send({
      content:
        `🗳️ 想定発話数 (${opts.discussion.expectedUtterances}) に到達しました ` +
        `(ラウンド${ctx.round} / 累計${ctx.total}発話)。議論を続けますか?\n` +
        `続ける場合、司会を除くメンバーを半数入れ替えます。`,
      components: [row],
    });

    return new Promise<boolean>((resolve) => {
      const timer = setTimeout(() => {
        pending.delete(token);
        resolve(false); // 無応答は停止
      }, opts.discussion.continueTimeoutMs);
      pending.set(token, { resolve, timer });
    });
  }

  async function start(channelId: string, topic: string): Promise<void> {
    active++;
    try {
      await postAs(channelId, "司会", `📣 議論を始めます: 「${topic}」`);
      const deps: DirectorDeps = {
        llm: { invoke: (a) => llm.invoke(a) },
        postUtterance: async (m, text) => {
          await postAs(channelId, m.personaName, text);
        },
        postSummary: async (m, text) => {
          await postAs(channelId, m.personaName, `🧷 まとめ: ${text}`);
        },
        askContinue: (ctx) => askContinue(channelId, ctx),
        log: (msg) => console.log(`  [debate ${channelId}] ${msg}`),
        rng: Math.random,
        turnDelayMs: opts.discussion.turnDelayMs,
        maxRounds: opts.discussion.maxRounds,
      };
      const result = await runDiscussion({ topic, partyConfig: partyConfig() }, deps);
      console.log(
        `  [debate ${channelId}] 終了 (${result.stoppedBy}, ${result.rounds}ラウンド, ${result.utterances.length}発話)`
      );
    } catch (err) {
      console.warn(`  [debate ${channelId}] 失敗: ${(err as Error).message}`);
    } finally {
      active--;
    }
  }

  async function handleButton(interaction: ButtonInteraction): Promise<boolean> {
    const id = interaction.customId;
    if (!id.startsWith(`${CUSTOM_PREFIX}:`)) return false;
    const [, kind, token] = id.split(":");
    const p = pending.get(token);
    if (!p) {
      await interaction.reply({ content: "この問いかけは期限切れです。", ephemeral: true }).catch(() => {});
      return true;
    }
    clearTimeout(p.timer);
    pending.delete(token);
    const cont = kind === "cont";
    p.resolve(cont);
    await interaction
      .update({
        content: cont ? "✅ 続行します (メンバーを半数入れ替えます)" : "🛑 ここで議論を締めます",
        components: [],
      })
      .catch(() => {});
    return true;
  }

  return { start, handleButton, activeCount: () => active };
}
