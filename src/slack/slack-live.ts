/**
 * 新フロー議論エンジン (src/flow) の Slack live アダプタ。
 *
 * discord-live.ts の Slack 版。`dispatchFlow` を呼び、onUtterance を chat.postMessage に、
 * onVote を reactions.add + 集計テキスト投稿に結線する。1 スレッド (thread_ts) = 1 議論。
 *
 *   - discussion / improvement : 完走型。結論を bot 投稿で締める。
 *   - learning                 : 収集のみ。openCore (Core) が要る。
 *   - sparring                 : 対話継続型。session を thread_ts で保持し、
 *                                スレッド返信を submitUser へ橋渡しする。
 *
 * 表示名は composeDisplayName のペルソナ名のみ (Slack の user 名/ID は保存しない)。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { CascadeClients } from "../crawler/sentiment/cascade.js";
import type { createCore } from "../core/index.js";
import type { ContextVoice } from "../flow/discussion-paper.js";
import type { FlowTag } from "../flow/tags.js";
import type { FlowUtteranceRecord, VoteEvent } from "../flow/director.js";
import type { FlowRole, FlowStance } from "../flow/personas.js";
import { composeDisplayName } from "../flow/persona-display.js";
import { dispatchFlow, type DispatchDeps, type FlowKind } from "../flow/dispatch.js";
import type { SparringSession } from "../flow/sparring.js";
import { postMessage, reactionsAdd } from "./web-api.js";

type Core = ReturnType<typeof createCore>;

/** 投票の可視化に使うリアクション名 (絵文字名、コロン無し)。 */
const VOTE_WINNER_REACTION = "trophy";
const VOTE_REACTION = "+1";

export interface SlackFlowDeps {
  /** Web API 用 bot token (xoxb-)。 */
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

export interface StartSlackFlowInput {
  channel: string;
  /** スレッド ID (= 親メッセージ ts)。 */
  threadTs: string;
  /** スレッド = 議論の主題。 */
  theme: string;
  flow: FlowKind;
  tags: FlowTag[];
  /** discussion/improvement のラウンド数 (任意)。 */
  rounds?: number;
  /** discussion/improvement の 1 ラウンドあたりターン数 (任意)。 */
  turnsPerRound?: number;
  /** 壁打ち相手のプールペルソナ id/name (任意。sparring のみ)。 */
  opponentPersonaIds?: string[];
}

/** 収束時フック (Discord 版 FlowLiveHooks と同形)。 */
export interface SlackFlowHooks {
  onConcluded?: (args: { scene: string; title: string; summary: string }) => void | Promise<void>;
}

/** 進行中の壁打ちセッション (thread_ts → session)。 */
const sparringByThread = new Map<string, SparringSession>();

/** テスト用: 登録セッションをクリアする。 */
export function _resetSlackLive(): void {
  sparringByThread.clear();
}

/** そのスレッドで進行中の壁打ちがあるか。 */
export function hasSlackSparringSession(threadTs: string): boolean {
  return sparringByThread.has(threadTs);
}

/** 1 議論ぶんの投稿コンテキスト (utterance_id → 投稿 ts / 露出名)。 */
interface ThreadPostCtx {
  /** utterance_id → 投稿した Slack message ts (リアクション付与先)。 */
  tsByUtterance: Map<string, string>;
  /** utterance_id → 露出名 (得票集計の表示用)。 */
  displayNameByUtterance: Map<string, string>;
}

function newThreadPostCtx(): ThreadPostCtx {
  return { tsByUtterance: new Map(), displayNameByUtterance: new Map() };
}

/**
 * スレッドへ persona 名で発話を流す onUtterance を作る。
 * username が効かない環境 (chat:write.customize 無し) に備え、本文先頭にも太字で表示名を出す。
 * 投稿 ts を ctx に控え、投票で使う。
 */
function makeThreadUtterancePoster(
  deps: SlackFlowDeps,
  channel: string,
  threadTs: string,
  ctx: ThreadPostCtx
) {
  return async (u: FlowUtteranceRecord): Promise<void> => {
    const displayName = composeDisplayName({
      name: u.personaName,
      stance: u.stance as FlowStance,
      role: u.role as FlowRole,
      possessionName: u.possessionName,
    });
    try {
      // username が無効でも誰の発話か分かるよう、本文先頭に表示名 (mrkdwn 太字) を付ける。
      const text = `*${displayName}*\n${u.text}`;
      const posted = await postMessage(deps.botToken, {
        channel,
        threadTs,
        username: displayName,
        iconEmoji: ":speech_balloon:",
        text,
      });
      if (posted.ts) ctx.tsByUtterance.set(u.id, posted.ts);
      ctx.displayNameByUtterance.set(u.id, displayName);
    } catch (err) {
      console.warn(`  slack-live: utterance post 失敗 (thread=${threadTs}): ${(err as Error).message}`);
    }
  };
}

/**
 * ラウンド投票の可視化。世論 (winner) に :trophy:、得票のあった発話に :+1: を付け、
 * 得票集計テキストをスレッドに投稿する。得票 0 なら何もしない。
 */
function makeThreadVoteReactor(deps: SlackFlowDeps, channel: string, threadTs: string, ctx: ThreadPostCtx) {
  return async (e: VoteEvent): Promise<void> => {
    const totalVotes = Object.values(e.tally).reduce((s, n) => s + n, 0);
    if (totalVotes === 0) return; // 全員棄権 → 可視化しない

    // 🏆/👍 を付与 (ts が取れている発話のみ)。
    for (const [utteranceId, votes] of Object.entries(e.tally)) {
      if (votes <= 0) continue;
      const ts = ctx.tsByUtterance.get(utteranceId);
      if (!ts) continue;
      const name = utteranceId === e.winner ? VOTE_WINNER_REACTION : VOTE_REACTION;
      await reactionsAdd(deps.botToken, { channel, timestamp: ts, name });
    }

    // 得票集計テキスト (降順)。
    const ranked = Object.entries(e.tally)
      .filter(([, n]) => n > 0)
      .sort((a, b) => b[1] - a[1])
      .map(([id, n], i) => {
        const name = ctx.displayNameByUtterance.get(id) ?? "(不明)";
        const medal = id === e.winner ? "🏆" : `${i + 1}.`;
        return `${medal} ${name} — ${n}票`;
      });
    if (ranked.length > 0) {
      await postThreadNotice(deps, channel, threadTs, `🗳️ *ラウンド ${e.round} 投票結果*\n${ranked.join("\n")}`);
    }
  };
}

/** bot 名義 (username 無し) でスレッドに 1 メッセージ投稿する (結論・案内用)。失敗は握る。 */
async function postThreadNotice(
  deps: SlackFlowDeps,
  channel: string,
  threadTs: string,
  text: string
): Promise<void> {
  try {
    await postMessage(deps.botToken, { channel, threadTs, text });
  } catch (err) {
    console.warn(`  slack-live: notice post 失敗 (thread=${threadTs}): ${(err as Error).message}`);
  }
}

function buildDispatchDeps(deps: SlackFlowDeps, channel: string, threadTs: string): DispatchDeps {
  const ctx = newThreadPostCtx();
  return {
    llm: deps.llm,
    listExternalVoices: deps.listExternalVoices,
    sentimentClients: deps.sentimentClients,
    gamesDir: deps.gamesDir,
    workspaceId: deps.workspaceId,
    onUtterance: makeThreadUtterancePoster(deps, channel, threadTs, ctx),
    onVote: makeThreadVoteReactor(deps, channel, threadTs, ctx),
    log: (m) => console.log(`  [slack-live ${threadTs}] ${m}`),
    warn: (m) => console.warn(`  [slack-live/warn ${threadTs}] ${m}`),
  };
}

/**
 * スレッド投稿を新フローで起動する。
 * discussion/improvement/learning は完走させ結論を投稿。sparring は session を登録して返信を待つ。
 * caller は `void startSlackFlow(...)` で呼ぶ (完走を待たない)。
 */
export async function startSlackFlow(
  input: StartSlackFlowInput,
  deps: SlackFlowDeps,
  hooks?: SlackFlowHooks
): Promise<void> {
  const scene = `slack:${input.channel}/${input.threadTs}`;
  const dispatchDeps = buildDispatchDeps(deps, input.channel, input.threadTs);
  const { channel, threadTs } = input;

  try {
    // ── 壁打ち: session を起動・登録し、以後のスレッド返信を submitUser に流す ──
    if (input.flow === "sparring") {
      const result = await dispatchFlow(
        { theme: input.theme, tags: input.tags, flow: input.flow, scene, opponentPersonaIds: input.opponentPersonaIds },
        dispatchDeps
      );
      if (result.kind !== "sparring") return;
      sparringByThread.set(threadTs, result.session);
      await postThreadNotice(
        deps,
        channel,
        threadTs,
        "🥊 *壁打ち* を始めます。あなたの主張・アイデアをこのスレッドに投稿してください。AI が反証・別視点で応答します。\n（`/終了` または十分話したら締めます）"
      );
      return;
    }

    // ── 学習: 収集のみ。Core が要る ──
    if (input.flow === "learning") {
      if (!deps.openCore) {
        await postThreadNotice(deps, channel, threadTs, "⚠️ 学習フローは Core 未設定のため起動できません。");
        return;
      }
      await postThreadNotice(deps, channel, threadTs, "📚 *学習* (外部の声の収集) を開始します…");
      const core = deps.openCore();
      try {
        const result = await dispatchFlow(
          { theme: input.theme, tags: input.tags, flow: input.flow, scene },
          { ...dispatchDeps, core }
        );
        if (result.kind === "learning") {
          await postThreadNotice(
            deps,
            channel,
            threadTs,
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
      channel,
      threadTs,
      `🗣️ *${input.flow === "improvement" ? "改善" : "議論"}* を始めます: 「${input.theme}」${
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
      await postThreadNotice(deps, channel, threadTs, `✅ *結論*\n${summary}`);
      await hooks?.onConcluded?.({ scene, title: input.theme, summary });
    }
  } catch (err) {
    console.warn(`  slack-live: ${input.flow} 起動失敗 (thread=${threadTs}): ${(err as Error).message}`);
    await postThreadNotice(deps, channel, threadTs, `⚠️ フロー実行中にエラーが発生しました: ${(err as Error).message}`);
  }
}

/**
 * 進行中の壁打ちスレッドへの返信を処理する。
 * @returns 壁打ちとして処理したら true (= 通常ルーティング不要)。
 */
export async function handleSlackFlowReply(
  threadTs: string,
  channel: string,
  text: string,
  deps: SlackFlowDeps,
  hooks?: SlackFlowHooks
): Promise<boolean> {
  const session = sparringByThread.get(threadTs);
  if (!session) return false;

  try {
    const result = await session.submitUser(text);
    if (result.kind === "ended") {
      sparringByThread.delete(threadTs);
      const summary = result.concluded ? result.conclusion : "(壁打ちを終了しました)";
      await postThreadNotice(deps, channel, threadTs, `✅ *壁打ち終了*\n${summary}`);
      await hooks?.onConcluded?.({ scene: `slack:${channel}/${threadTs}`, title: "壁打ち", summary });
    }
  } catch (err) {
    console.warn(`  slack-live: 壁打ち返信処理失敗 (thread=${threadTs}): ${(err as Error).message}`);
  }
  return true;
}
