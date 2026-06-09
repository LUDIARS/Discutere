/**
 * 進行役への調整指示の取り込み (transport 非依存)。
 *
 * gateway (discord.js) が bot メンション / bot・persona へのリプライを検知したら、
 * 本文 (メンション除去済) と発話場所 (guild/channel) をここへ渡す。 ここでは:
 *   1. scene "discord:<guild>/<channel>" から進行中の議論 (design gap) を引く
 *   2. 引けたら調整指示として gap に紐付けて保存する
 *   3. Discord に返す「了解」文 (ack) を組み立てて返す
 * を行う。 保存した指示は facilitator / persona prompt が次の tick/発話で読む。
 *
 * core (KG) の open/close はここに閉じ込める (command-router と同じ方針)。
 */

import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { getConfig } from "../config.js";
import {
  addFacilitatorDirective,
  normalizeDirectiveText,
  resolveActiveGapForScene,
} from "./facilitator-directives.js";

export interface DirectiveMessageInput {
  workspaceId: string;
  /** guild id (DM は "dm")。 */
  guildId: string;
  /** 発話チャンネル id (フォーラムは thread id)。 scene の解決に使う。 */
  channelId: string;
  /** 調整指示の本文 (bot メンション除去済)。 */
  text: string;
  /** 指示を出した Discord user id (監査用)。 */
  authorId?: string | null;
}

export interface DirectiveMessageResult {
  /** 指示として保存できたか (false なら gap 不在 or 空文 で未保存)。 */
  stored: boolean;
  /** Discord に返す ack 文 (常に非空。 失敗時は理由を返す)。 */
  reply: string;
  /** 保存先 gap id (stored=true のときのみ)。 */
  gapId?: string;
}

/**
 * 調整指示を取り込み、 Discord に返す ack 文を返す。
 * - 議論が立っていない → 保存せず案内文。
 * - 本文が空 (メンションのみ等) → 保存せず使い方を返す。
 */
export function handleDirectiveMessage(input: DirectiveMessageInput): DirectiveMessageResult {
  const normalized = normalizeDirectiveText(input.text);
  if (!normalized) {
    return {
      stored: false,
      reply:
        "🛠️ 進行の調整指示を受け付けます。 例:「もう少し簡単な言葉で話して」「もっと否定的な意見が欲しい」 のように、 メンションかリプライに続けて書いてください。",
    };
  }

  const core = createCore(resolveActiveKgPath(getConfig()));
  try {
    const scene = `discord:${input.guildId}/${input.channelId}`;
    const gapId = resolveActiveGapForScene(core.client.raw, input.workspaceId, scene);
    if (!gapId) {
      return {
        stored: false,
        reply:
          "🛠️ いま調整できる進行中の議論が見つかりませんでした。 議論が立ってから、 そのスレッド/チャンネルで指示してください。",
      };
    }
    addFacilitatorDirective(core.client.raw, {
      workspaceId: input.workspaceId,
      gapId,
      text: normalized,
      authorId: input.authorId ?? null,
    });
    return {
      stored: true,
      gapId,
      reply: `🛠️ 了解です。「${normalized}」 を進行に反映します。 これからの発言とまとめに効いてきます。`,
    };
  } finally {
    core.close();
  }
}
