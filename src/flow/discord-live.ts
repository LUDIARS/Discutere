/**
 * 新フロー議論エンジン (src/flow) の Discord live アダプタ。
 *
 * 旧 auto-discussion (persona-engine + design_gap) に代わり、フォーラム議論を
 * `dispatchFlow` (議論/改善/学習/壁打ち) で回す。`onUtterance` を webhook 投稿に
 * 繋いで発話を persona 名でスレッドに流し、結論を bot 名義で締める。
 *
 *   - discussion / improvement : 完走型。ラウンドを回して結論を投稿。
 *   - learning                 : 収集のみ (LLM 議論なし)。Core が要る。
 *   - sparring                 : 対話継続型。session を threadId で保持し、
 *                                スレッドへの返信を submitUser へ橋渡しする。
 *
 * 出力先 (webhook / bot post) は poster.ts に委譲し、フローのロジックは dispatch に委譲する。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { CascadeClients } from "../crawler/sentiment/cascade.js";
import type { createCore } from "../core/index.js";
import type { ContextVoice } from "./discussion-paper.js";
import type { FlowTag } from "./tags.js";
import type { FlowUtteranceRecord } from "./director.js";
import { dispatchFlow, type DispatchDeps, type FlowKind } from "./dispatch.js";
import type { SparringSession } from "./sparring.js";
import {
  ensureChannelWebhook,
  humanizeForDiscord,
  postDiscordChannel,
  postDiscordWebhook,
  resolveWebhookTarget,
} from "../discord-hook/poster.js";

type Core = ReturnType<typeof createCore>;

export interface FlowDiscordDeps {
  /** webhook / message 投稿用 bot token。 */
  botToken: string;
  /** フロー実行用 LLM クライアント。 */
  llm: LLMClient;
  /** learning が KG 書込に使う Core ファクトリ (learning を許可するなら必須)。 */
  openCore?: () => Core;
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  sentimentClients?: CascadeClients;
  gamesDir?: string;
  workspaceId?: string;
}

export interface StartForumFlowInput {
  guildId: string;
  threadId: string;
  /** スレッド = 議論の主題。 */
  theme: string;
  flow: FlowKind;
  tags: FlowTag[];
  /** 議論ごとのラウンド数 (任意。discussion/improvement のみ反映、未指定は config 既定)。 */
  rounds?: number;
  /** 議論ごとの 1 ラウンドあたりターン数 (任意。discussion/improvement のみ反映)。 */
  turnsPerRound?: number;
}

/** 収束時フック (gateway が finalizeForumPost に結線する)。 */
export interface FlowLiveHooks {
  onConcluded?: (args: { scene: string; title: string; summary: string }) => void | Promise<void>;
}

/** 進行中の壁打ちセッション (threadId → session)。スレッド返信を submitUser へ橋渡しする。 */
const sparringByThread = new Map<string, SparringSession>();

/** テスト用: 登録セッションをクリアする。 */
export function _resetFlowLive(): void {
  sparringByThread.clear();
}

/** そのスレッドで進行中の壁打ちがあるか。 */
export function hasSparringSession(threadId: string): boolean {
  return sparringByThread.has(threadId);
}

/** フォーラムスレッドへ persona 名で発話を流す onUtterance を作る。 */
function makeThreadUtterancePoster(deps: FlowDiscordDeps, threadId: string) {
  return async (u: FlowUtteranceRecord): Promise<void> => {
    try {
      const target = await resolveWebhookTarget(deps.botToken, threadId);
      const wh = await ensureChannelWebhook(deps.botToken, target.webhookChannelId);
      const text = u.isError ? u.text : humanizeForDiscord(u.text);
      await postDiscordWebhook({
        webhookId: wh.id,
        webhookToken: wh.token,
        username: u.personaName,
        content: text,
        threadId: target.threadId ?? threadId,
      });
    } catch (err) {
      console.warn(`  flow-live: utterance post 失敗 (thread=${threadId}): ${(err as Error).message}`);
    }
  };
}

/** bot 名義でスレッドに 1 メッセージ投稿する (結論・案内用)。失敗は握り潰す。 */
async function postThreadNotice(deps: FlowDiscordDeps, threadId: string, content: string): Promise<void> {
  try {
    await postDiscordChannel({ botToken: deps.botToken, channelId: threadId, content });
  } catch (err) {
    console.warn(`  flow-live: notice post 失敗 (thread=${threadId}): ${(err as Error).message}`);
  }
}

function buildDispatchDeps(deps: FlowDiscordDeps, threadId: string): DispatchDeps {
  return {
    llm: deps.llm,
    listExternalVoices: deps.listExternalVoices,
    sentimentClients: deps.sentimentClients,
    gamesDir: deps.gamesDir,
    workspaceId: deps.workspaceId,
    onUtterance: makeThreadUtterancePoster(deps, threadId),
    log: (m) => console.log(`  [flow-live ${threadId}] ${m}`),
    warn: (m) => console.warn(`  [flow-live/warn ${threadId}] ${m}`),
  };
}

/**
 * フォーラム投稿を新フローで起動する。
 * discussion/improvement/learning は完走させ結論を投稿。sparring は session を登録して返信を待つ。
 * gateway は `void startForumFlow(...)` で呼ぶ (完走を待たない)。
 */
export async function startForumFlow(
  input: StartForumFlowInput,
  deps: FlowDiscordDeps,
  hooks?: FlowLiveHooks
): Promise<void> {
  const scene = `discord:${input.guildId}/${input.threadId}`;
  const dispatchDeps = buildDispatchDeps(deps, input.threadId);

  try {
    // ── 壁打ち: session を起動・登録し、以後のスレッド返信を submitUser に流す ──
    if (input.flow === "sparring") {
      const result = await dispatchFlow(
        { theme: input.theme, tags: input.tags, flow: input.flow, scene },
        dispatchDeps
      );
      if (result.kind !== "sparring") return;
      sparringByThread.set(input.threadId, result.session);
      await postThreadNotice(
        deps,
        input.threadId,
        "🥊 **壁打ち**を始めます。あなたの主張・アイデアを投稿してください。AI が反証・別視点で応答します。\n（`/終了` または十分話したら締めます）"
      );
      return;
    }

    // ── 学習: 収集のみ。Core が要る ──
    if (input.flow === "learning") {
      if (!deps.openCore) {
        await postThreadNotice(deps, input.threadId, "⚠️ 学習フローは Core 未設定のため起動できません。");
        return;
      }
      await postThreadNotice(deps, input.threadId, "📚 **学習** (外部の声の収集) を開始します…");
      const core = deps.openCore();
      try {
        const result = await dispatchFlow(
          { theme: input.theme, tags: input.tags, flow: input.flow, scene },
          { ...dispatchDeps, core }
        );
        if (result.kind === "learning") {
          await postThreadNotice(
            deps,
            input.threadId,
            `✅ 学習完了: 「${input.theme}」を KG に取り込みました (slug=${result.result.gameSlug}).`
          );
        }
      } finally {
        core.close?.();
      }
      return;
    }

    // ── 議論 / 改善: 完走させて結論を投稿 ──
    await postThreadNotice(
      deps,
      input.threadId,
      `🗣️ **${input.flow === "improvement" ? "改善" : "議論"}**を始めます: 「${input.theme}」${
        input.tags.length ? `\nタグ: ${input.tags.join(" / ")}` : ""
      }`
    );
    const result = await dispatchFlow(
      {
        theme: input.theme,
        tags: input.tags,
        flow: input.flow,
        scene,
        rounds: input.rounds,
        turnsPerRound: input.turnsPerRound,
      },
      dispatchDeps
    );
    if (result.kind === "discussion" || result.kind === "improvement") {
      const r = result.result;
      const summary = r.concluded ? r.conclusion : "(明確な結論には至りませんでした)";
      await postThreadNotice(deps, input.threadId, `✅ **結論**\n${summary}`);
      await hooks?.onConcluded?.({ scene, title: input.theme, summary });
    }
  } catch (err) {
    console.warn(`  flow-live: ${input.flow} 起動失敗 (thread=${input.threadId}): ${(err as Error).message}`);
    await postThreadNotice(deps, input.threadId, `⚠️ フロー実行中にエラーが発生しました: ${(err as Error).message}`);
  }
}

/**
 * 進行中の壁打ちスレッドへの返信を処理する。
 * @returns 壁打ちとして処理したら true (= 通常ルーティング不要)。
 */
export async function handleForumFlowReply(
  threadId: string,
  guildId: string,
  text: string,
  deps: FlowDiscordDeps,
  hooks?: FlowLiveHooks
): Promise<boolean> {
  const session = sparringByThread.get(threadId);
  if (!session) return false;

  try {
    const result = await session.submitUser(text);
    if (result.kind === "ended") {
      sparringByThread.delete(threadId);
      const summary = result.concluded ? result.conclusion : "(壁打ちを終了しました)";
      await postThreadNotice(deps, threadId, `✅ **壁打ち終了**\n${summary}`);
      await hooks?.onConcluded?.({ scene: `discord:${guildId}/${threadId}`, title: "壁打ち", summary });
    }
  } catch (err) {
    console.warn(`  flow-live: 壁打ち返信処理失敗 (thread=${threadId}): ${(err as Error).message}`);
  }
  return true;
}
