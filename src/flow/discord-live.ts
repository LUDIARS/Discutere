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
import type { FlowUtteranceRecord, VoteEvent, PaperOverride } from "./director.js";
import type { FlowRole, FlowStance } from "./personas.js";
import { composeDisplayName } from "./persona-display.js";
import { dispatchFlow, type DispatchDeps, type FlowKind } from "./dispatch.js";
import { ensureLearningData, isAutoCrawlSource, resolveAutoCrawlSources, deriveSlug } from "./learning-autocrawl.js";
import { analyzeSpecMechanics } from "./spec-analyze.js";
import { resolveSpecText } from "./spec-source.js";
import type { GameMechanicEntry } from "./games-md.js";
import { gateBeforeFlow, resolveDebatabilityGate } from "./information-gate-runner.js";
import {
  buildPaperDraft,
  applyPaperEdit,
  renderPaperReview,
  isApprovalText,
  isRevertText,
  withDerivedStructure,
  type PaperDraft,
  type PaperReviewInfo,
} from "./paper-review.js";
import { persistDraftPaper, getDraftPaper, setPaperDebatability, deleteFlowSession } from "./discussion-paper.js";
import { appendRevision, revertLast, canRevert } from "./paper-revisions.js";
import { getConfig } from "../config.js";
import type { SparringSession } from "./sparring.js";
import {
  ensureChannelWebhook,
  humanizeForDiscord,
  postDiscordChannel,
  postDiscordWebhook,
  reactDiscord,
  resolveWebhookTarget,
} from "../discord-hook/poster.js";

/** 投票の可視化に使う絵文字 (item3)。 */
const VOTE_WINNER_EMOJI = "🏆";
const VOTE_EMOJI = "👍";

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
  youtubeApiKey?: string | null;
  getYoutubeApiKey?: () => Promise<string | null>;
}

async function resolveYoutubeApiKey(deps: FlowDiscordDeps): Promise<string | null> {
  return deps.getYoutubeApiKey ? await deps.getYoutubeApiKey() : deps.youtubeApiKey ?? null;
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
  /** 壁打ち相手のプールペルソナ id/name (任意。sparring のみ反映)。 */
  opponentPersonaIds?: string[];
  /** starter 本文 (任意。学習で仕様書解析 ② に使う。タイトルと別物のときだけ渡る)。 */
  specText?: string;
  /** starter のテキスト系添付ファイル URL (任意。学習で仕様書解析 ③ に使う)。 */
  specAttachmentUrls?: string[];
}

/** 収束時フック (gateway が finalizeForumPost に結線する)。 */
export interface FlowLiveHooks {
  onConcluded?: (args: { scene: string; title: string; summary: string }) => void | Promise<void>;
  /**
   * 議論適性ゲート (09) が「議論不適」を出したときのフロー再提案フック。
   * gateway が既存の議論タイプ選択メニュー (flow-pick) の再提示に結線する。
   */
  onReproposeFlowType?: (args: {
    guildId: string;
    threadId: string;
    theme: string;
    tags: FlowTag[];
  }) => void | Promise<void>;
}

/** 進行中の壁打ちセッション (threadId → session)。スレッド返信を submitUser へ橋渡しする。 */
const sparringByThread = new Map<string, SparringSession>();

/** レビュー待ちのディスカッションペーパー (threadId → 草案 + 起動入力)。スレッド返信で調整/承認する。 */
interface PendingPaperReview {
  input: StartForumFlowInput;
  draft: PaperDraft;
  info: PaperReviewInfo;
  hooks?: FlowLiveHooks;
  /** 無操作の自動開始タイマー (timeoutMs>0 時のみ。承認/調整で張り直す)。 */
  timer?: ReturnType<typeof setTimeout>;
}
const paperReviewByThread = new Map<string, PendingPaperReview>();

/** テスト用: 登録セッションをクリアする。 */
export function _resetFlowLive(): void {
  sparringByThread.clear();
  for (const p of paperReviewByThread.values()) if (p.timer) clearTimeout(p.timer);
  paperReviewByThread.clear();
}

/** そのスレッドで進行中の壁打ちがあるか。 */
export function hasSparringSession(threadId: string): boolean {
  return sparringByThread.has(threadId);
}

/** そのスレッドでペーパーレビュー待ちか (メモリ + 永続ドラフトの両方を見る)。 */
export function hasPaperReview(threadId: string): boolean {
  return paperReviewByThread.has(threadId) || getDraftPaper(threadId) !== null;
}

/** 1 議論ぶんの投稿コンテキスト (utterance_id → 投稿 message_id を保持し、投票で参照する)。 */
interface ThreadPostCtx {
  /** utterance_id → 投稿した Discord message_id (リアクション付与先)。 */
  messageIdByUtterance: Map<string, string>;
  /** utterance_id → 露出名 (得票集計の表示用)。 */
  displayNameByUtterance: Map<string, string>;
}

function newThreadPostCtx(): ThreadPostCtx {
  return { messageIdByUtterance: new Map(), displayNameByUtterance: new Map() };
}

/**
 * フォーラムスレッドへ persona 名で発話を流す onUtterance を作る。
 * 露出名は「名前 (ロール/憑依ペルソナ)」(item2/4)。投稿 message_id を ctx に控え、投票で使う (item3)。
 */
function makeThreadUtterancePoster(deps: FlowDiscordDeps, threadId: string, ctx: ThreadPostCtx) {
  return async (u: FlowUtteranceRecord): Promise<void> => {
    const displayName = composeDisplayName({
      name: u.personaName,
      stance: u.stance as FlowStance,
      role: u.role as FlowRole,
      possessionName: u.possessionName,
    });
    try {
      const target = await resolveWebhookTarget(deps.botToken, threadId);
      const wh = await ensureChannelWebhook(deps.botToken, target.webhookChannelId);
      const text = u.isError ? u.text : humanizeForDiscord(u.text);
      const posted = await postDiscordWebhook({
        webhookId: wh.id,
        webhookToken: wh.token,
        username: displayName,
        content: text,
        threadId: target.threadId ?? threadId,
      });
      if (posted.id) ctx.messageIdByUtterance.set(u.id, posted.id);
      ctx.displayNameByUtterance.set(u.id, displayName);
    } catch (err) {
      console.warn(`  flow-live: utterance post 失敗 (thread=${threadId}): ${(err as Error).message}`);
    }
  };
}

/**
 * ラウンド投票の可視化 (item3)。bot は同一ユーザなので「得票数ぶんのリアクション」は付けられないため、
 *  - 世論 (winner) の発話に 🏆、得票のあった発話に 👍 を付け、
 *  - 得票集計テキストをスレッドに投稿する (誰の意見が何票か)。
 */
function makeThreadVoteReactor(deps: FlowDiscordDeps, threadId: string, ctx: ThreadPostCtx) {
  return async (e: VoteEvent): Promise<void> => {
    const totalVotes = Object.values(e.tally).reduce((s, n) => s + n, 0);
    if (totalVotes === 0) return; // 全員棄権 → 可視化しない

    // 🏆/👍 を付与 (message_id が取れている発話のみ)。
    for (const [utteranceId, votes] of Object.entries(e.tally)) {
      if (votes <= 0) continue;
      const messageId = ctx.messageIdByUtterance.get(utteranceId);
      if (!messageId) continue;
      const emoji = utteranceId === e.winner ? VOTE_WINNER_EMOJI : VOTE_EMOJI;
      try {
        await reactDiscord({ botToken: deps.botToken, channelId: threadId, messageId, emoji });
      } catch (err) {
        console.warn(`  flow-live: 投票リアクション失敗 (thread=${threadId}): ${(err as Error).message}`);
      }
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
      await postThreadNotice(deps, threadId, `🗳️ **ラウンド ${e.round} 投票結果**\n${ranked.join("\n")}`);
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
  const ctx = newThreadPostCtx();
  return {
    llm: deps.llm,
    listExternalVoices: deps.listExternalVoices,
    sentimentClients: deps.sentimentClients,
    gamesDir: deps.gamesDir,
    workspaceId: deps.workspaceId,
    onUtterance: makeThreadUtterancePoster(deps, threadId, ctx),
    onVote: makeThreadVoteReactor(deps, threadId, ctx),
    log: (m) => console.log(`  [flow-live ${threadId}] ${m}`),
    warn: (m) => console.warn(`  [flow-live/warn ${threadId}] ${m}`),
  };
}

/**
 * 議論/改善の開始前に情報を整える。
 *   1. 情報ゲート (LLM が情報密度を評価し、不足観点を狙って学習 → 再評価) が有効ならそれを実行し、
 *      学習が走ったらスレッドに進捗を通知する。
 *   2. ゲート対象外なら、従来のカウント閾値 autoCrawl にフォールバックする。
 * 失敗は議論を止めない (graceful)。
 */
async function prepareInformationBeforeForumFlow(
  input: StartForumFlowInput,
  deps: FlowDiscordDeps
): Promise<void> {
  // 情報ゲート優先 (LLM 評価 + 不足観点クロール)。Discord は scene=threadId をコストログキーにする。
  try {
    const youtubeApiKey = await resolveYoutubeApiKey(deps);
    const gate = await gateBeforeFlow({
      kind: input.flow,
      theme: input.theme,
      tags: input.tags,
      llm: deps.llm,
      openCore: deps.openCore,
      workspaceId: deps.workspaceId ?? getConfig().workspace,
      listExternalVoices: deps.listExternalVoices,
      sessionId: input.threadId,
      log: (m) => console.log(`  [forum-gate ${input.threadId}] ${m}`),
      warn: (m) => console.warn(`  [forum-gate ${input.threadId}] ${m}`),
      youtubeApiKey,
    });
    if (gate) {
      // 学習が走った時のみ通知 (充分で no-op の時は黙る)。
      if (gate.crawls > 0) {
        await postThreadNotice(deps, input.threadId, `📊 ${gate.message}`);
      }
      return;
    }
  } catch (e) {
    console.warn(
      `  [forum-gate ${input.threadId}] 情報ゲート失敗 (議論は続行): ${(e as Error).message}`
    );
  }
  // フォールバック: 従来のカウント閾値 autoCrawl。
  await legacyAutoCrawlBeforeForumFlow(input, deps);
}

/**
 * 旧来の自動クロール (テーマの学習データが不足していれば config 既定ソースでクロール → 取込)。
 * Discord フォーラムは投稿ごとのパラメータ UI が無いため既定ソース
 * (config.flow.autoCrawl.source) のみを使う。失敗は議論を止めない (graceful)。
 */
async function legacyAutoCrawlBeforeForumFlow(
  input: StartForumFlowInput,
  deps: FlowDiscordDeps
): Promise<void> {
  const cfg = getConfig().flow.autoCrawl;
  if (!cfg.enabled || !deps.openCore || !isAutoCrawlSource(cfg.source)) return;
  const core = deps.openCore();
  try {
    const youtubeApiKey = await resolveYoutubeApiKey(deps);
    const result = await ensureLearningData({
      core,
      theme: input.theme,
      slug: deriveSlug(input.theme),
      workspaceId: deps.workspaceId ?? getConfig().workspace,
      spec: { source: cfg.source },
      minVoices: cfg.minVoices,
      maxItems: cfg.maxItems,
      listExternalVoices: deps.listExternalVoices,
      youtubeApiKey: youtubeApiKey ?? undefined,
      log: (m) => console.log(`  [forum-autocrawl ${input.threadId}] ${m}`),
      warn: (m) => console.warn(`  [forum-autocrawl ${input.threadId}] ${m}`),
    });
    if (result.crawled) {
      await postThreadNotice(deps, input.threadId, `📚 ${result.message}`);
    }
  } catch (e) {
    console.warn(
      `  [forum-autocrawl ${input.threadId}] クロール失敗 (議論は続行): ${(e as Error).message}`
    );
  } finally {
    core.close?.();
  }
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
        { theme: input.theme, tags: input.tags, flow: input.flow, scene, opponentPersonaIds: input.opponentPersonaIds },
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
      // 自動収集モード: config.flow.autoCrawl の自動経路ソース横断でテーマをクロール → KG 取込。
      const crawlCfg = getConfig().flow.autoCrawl;
      const youtubeApiKey = (await resolveYoutubeApiKey(deps)) ?? undefined;
      const crawlSources = crawlCfg.enabled
        ? resolveAutoCrawlSources(crawlCfg.sources, youtubeApiKey)
        : [];
      const learningCrawl =
        crawlSources.length > 0
          ? { sources: crawlSources, maxItems: crawlCfg.maxItems, youtubeApiKey }
          : undefined;
      // 仕様書解析 (②/③): starter 本文 (specText) + テキスト系添付ファイル (specAttachmentUrls) を
      // まとめて LLM 解析 → mechanics として記録。添付は URL 取得 (ローカルパス読みは不許可)。
      const specParts: string[] = [];
      if (input.specText) specParts.push(input.specText);
      for (const url of input.specAttachmentUrls ?? []) {
        try {
          specParts.push(await resolveSpecText(url, { allowLocalPath: false }));
        } catch (e) {
          console.warn(`  [spec-analyze ${input.threadId}] 添付取得失敗 (${url}): ${(e as Error).message}`);
        }
      }
      let mechanics: GameMechanicEntry[] | undefined;
      if (specParts.length > 0) {
        mechanics = await analyzeSpecMechanics({
          theme: input.theme,
          specText: specParts.join("\n\n"),
          llm: deps.llm,
          warn: (m) => console.warn(`  [spec-analyze ${input.threadId}] ${m}`),
        });
      }
      const core = deps.openCore();
      try {
        const result = await dispatchFlow(
          { theme: input.theme, tags: input.tags, flow: input.flow, scene },
          { ...dispatchDeps, core, learningCrawl, mechanics }
        );
        if (result.kind === "learning") {
          const r = result.result;
          const collected = r.opinionsRecorded + r.crawledImported + r.mechanicsRecorded;
          const parts: string[] = [];
          if (r.mechanicsRecorded > 0) parts.push(`仕様書メカニクス ${r.mechanicsRecorded} 件`);
          if (r.crawledImported > 0)
            parts.push(`自動収集 ${r.crawledImported} 件 (${Object.keys(r.crawledBySource).join("/")})`);
          const detail = parts.length > 0 ? ` (${parts.join(" / ")})` : "";
          await postThreadNotice(
            deps,
            input.threadId,
            collected > 0
              ? `✅ 学習完了: 「${input.theme}」を KG に ${collected} 件取り込みました${detail} (slug=${r.gameSlug}).`
              : `✅ 学習完了: 「${input.theme}」(取込 0 件。収集元が見つからないか既出です。slug=${r.gameSlug}).`
          );
        }
      } finally {
        core.close?.();
      }
      return;
    }

    // ── 議論 / 改善 ──
    const flowLabel = input.flow === "improvement" ? "改善" : "議論";
    // 議論前の情報ゲート (LLM 密度評価 + 不足観点学習) / フォールバック autoCrawl で材料を整える。
    const reviewEnabled = getConfig().flow.paperReview.enabled;
    await postThreadNotice(
      deps,
      input.threadId,
      `🗣️ **${flowLabel}**の${reviewEnabled ? "準備をしています" : "を始めます"}: 「${input.theme}」${
        input.tags.length ? `\nタグ: ${input.tags.join(" / ")}` : ""
      }`
    );
    await prepareInformationBeforeForumFlow(input, deps);

    // ペーパーレビューゲート (有効時): 草案 + 集めた情報を出し、調整/承認をスレッド返信で待つ。
    if (reviewEnabled) {
      await startPaperReview(input, deps, hooks);
      return;
    }

    // 通常: そのまま完走させて結論を投稿。
    await runDiscussionDispatch(input, deps, hooks);
  } catch (err) {
    console.warn(`  flow-live: ${input.flow} 起動失敗 (thread=${input.threadId}): ${(err as Error).message}`);
    await postThreadNotice(deps, input.threadId, `⚠️ フロー実行中にエラーが発生しました: ${(err as Error).message}`);
  }
}

/**
 * ペーパー草案 (議題ブリーフ + 集めた情報) を作ってスレッドに出し、レビュー待ちに登録する。
 * 以後のスレッド返信は handlePaperReviewReply が「調整」または「承認 (=議論開始)」として処理する。
 */
async function startPaperReview(
  input: StartForumFlowInput,
  deps: FlowDiscordDeps,
  hooks?: FlowLiveHooks
): Promise<void> {
  const richness = getConfig().flow.paperRichness;
  const { draft, info } = await buildPaperDraft(input.theme, input.tags, {
    gamesDir: deps.gamesDir,
    listExternalVoices: deps.listExternalVoices,
    llm: richness.enrichMechanics ? deps.llm : undefined,
    mechanicsTarget: richness.mechanicsTarget,
    enrichModel: richness.enrichModel || undefined,
    // 議論適性ゲート (09): 情報ゲートの後段・人間レビューの前 (無効時は undefined = 現行挙動)。
    debatability: resolveDebatabilityGate({ kind: input.flow, sessionId: input.threadId, llm: deps.llm }),
    warn: (m) => console.warn(`  [paper-review ${input.threadId}] ${m}`),
  });
  paperReviewByThread.set(input.threadId, { input, draft, info, hooks });
  // ドラフトを discussion_paper(status='draft') に永続 → 議論一覧に「下書き」として出す/再開できる
  // (session_id=threadId。承認時に同 session 行を 'started' へ upsert する)。
  persistDiscordDraft(input, draft);
  appendRevision({ sessionId: input.threadId, bodyMd: draft.bodyMd, changeSummary: "初期草案", origin: "initial" });
  await postThreadNotice(deps, input.threadId, renderPaperReview(draft, info));
  await postThreadNotice(deps, input.threadId, approvalGuide());
  // 議論不適 → フロー再提案 (09): 提案リプライ + 議論タイプ選択メニュー再提示 (hook 経由)。
  // レビュー待ちは維持する (「開始」で強行も可 — 人間が最終決定)。
  const d = info.debatability;
  if (d && !d.degraded && !d.debatable && d.recommendation) {
    const label = d.recommendation.flow === "sparring" ? "壁打ち" : "学習";
    await postThreadNotice(
      deps,
      input.threadId,
      `💡 **フロー再提案**: このテーマは「${label}」が向いています。${d.recommendation.reason}\n` +
        "下のメニューで議論タイプを選び直すか、このまま **「開始」** で議論を強行できます。"
    );
    try {
      await hooks?.onReproposeFlowType?.({
        guildId: input.guildId,
        threadId: input.threadId,
        theme: input.theme,
        tags: input.tags,
      });
    } catch (e) {
      console.warn(`  [paper-review ${input.threadId}] タイプ選択メニュー再提示失敗: ${(e as Error).message}`);
    }
  }
  scheduleReviewAutoStart(input.threadId, deps);
}

/** Discord レビュー草案を discussion_paper(status='draft') に永続/同期する (session_id=threadId)。 */
function persistDiscordDraft(input: StartForumFlowInput, draft: PaperDraft): void {
  persistDraftPaper(
    {
      sessionId: input.threadId,
      theme: draft.theme,
      tags: draft.tags,
      mechanics: draft.mechanics,
      supplement: draft.supplement,
      bodyMd: draft.bodyMd,
    },
    input.flow
  );
}

/** レビュー承認の案内文 (タイムアウト設定があれば併記)。 */
function approvalGuide(): string {
  const timeoutMs = getConfig().flow.paperReview.timeoutMs;
  const base =
    "✏️ 調整があればこのスレッドに返信してください (例:「メカニクスにガチャを追加」「観点補足を初心者向けに」)。\n" +
    "よければ **「開始」** と返信するか ✅ を付けると議論を始めます。";
  if (timeoutMs > 0) {
    const min = Math.round(timeoutMs / 60000);
    return `${base}\n(${min > 0 ? `${min} 分` : `${Math.round(timeoutMs / 1000)} 秒`}無操作なら草案のまま自動で始めます)`;
  }
  return base;
}

/** 無操作の自動開始タイマーを (張り直して) 仕掛ける。timeoutMs<=0 なら何もしない。 */
function scheduleReviewAutoStart(threadId: string, deps: FlowDiscordDeps): void {
  const pending = paperReviewByThread.get(threadId);
  if (!pending) return;
  const timeoutMs = getConfig().flow.paperReview.timeoutMs;
  if (timeoutMs <= 0) return;
  if (pending.timer) clearTimeout(pending.timer);
  pending.timer = setTimeout(() => {
    void (async () => {
      const p = paperReviewByThread.get(threadId);
      if (!p) return;
      paperReviewByThread.delete(threadId);
      await postThreadNotice(deps, threadId, "⏱️ 無操作のため草案のまま議論を始めます…");
      recordForcedDebatability(threadId, p.info);
      await runDiscussionDispatch(p.input, deps, p.hooks, overrideFromPending(p));
    })().catch((err) =>
      console.warn(`  flow-live: ペーパー自動開始失敗 (thread=${threadId}): ${(err as Error).message}`)
    );
  }, timeoutMs);
  pending.timer.unref?.();
}

/** レビュー待ちの草案から runFlow へ渡す確定ペーパー (issues / 議論適性サマリ込み) を組む。 */
function overrideFromPending(p: PendingPaperReview): PaperOverride {
  const d = p.info.debatability;
  return {
    mechanics: p.draft.mechanics,
    supplement: p.draft.supplement,
    bodyMd: p.draft.bodyMd,
    // 承認済み論点 (09): ファシリテーター開幕プロンプトの参考として運ぶ。
    issues: p.draft.issues,
    ...(d && !d.degraded
      ? { debatability: { debatable: d.debatable, armableBothCount: d.armableBothCount } }
      : {}),
  };
}

/** 議論不適のまま強行したとき、評価結果を discussion_paper に記録する (09 監査ログ)。 */
function recordForcedDebatability(threadId: string, info: PaperReviewInfo): void {
  const d = info.debatability;
  if (!d || d.degraded || d.debatable) return;
  try {
    setPaperDebatability(threadId, d);
  } catch (e) {
    console.warn(`  [paper-review ${threadId}] debatability 記録失敗 (議論は続行): ${(e as Error).message}`);
  }
}

/** 確定ペーパー (任意) で議論/改善を完走させ、結論を投稿する。 */
async function runDiscussionDispatch(
  input: StartForumFlowInput,
  deps: FlowDiscordDeps,
  hooks?: FlowLiveHooks,
  paperOverride?: PaperOverride
): Promise<void> {
  const scene = `discord:${input.guildId}/${input.threadId}`;
  const dispatchDeps = buildDispatchDeps(deps, input.threadId);
  const result = await dispatchFlow(
    {
      theme: input.theme,
      tags: input.tags,
      flow: input.flow,
      scene,
      rounds: input.rounds,
      turnsPerRound: input.turnsPerRound,
    },
    // session_id=threadId: 編集ゲートの draft 行を 'started' に upsert する (重複行を作らない)。
    { ...dispatchDeps, sessionId: input.threadId, paperOverride }
  );
  if (result.kind === "discussion" || result.kind === "improvement") {
    const r = result.result;
    const summary = r.concluded ? r.conclusion : "(明確な結論には至りませんでした)";
    await postThreadNotice(deps, input.threadId, `✅ **結論**\n${summary}`);
    await hooks?.onConcluded?.({ scene, title: input.theme, summary });
  }
}

/**
 * ペーパーレビュー中のスレッド返信を処理する (壁打ちより優先で呼ぶ)。
 *   - 承認語 (「開始」等) → 確定ペーパーで議論を開始する。
 *   - それ以外 → 自然文の調整指示として LLM でペーパーに反映し、更新版を再掲する。
 * @returns レビュー返信として処理したら true (= 通常ルーティング不要)。
 */
/**
 * 永続ドラフト (status='draft') からレビュー待ちをメモリに復元する (再起動/プロセス跨ぎの再開)。
 * guildId は返信コンテキストから来た値を使う (draft 行に guildId は持たない)。無ければ null。
 */
function rehydratePaperReview(
  threadId: string,
  guildId: string,
  hooks?: FlowLiveHooks
): PendingPaperReview | null {
  const row = getDraftPaper(threadId);
  if (!row) return null;
  const draft = withDerivedStructure(row.bodyMd, {
    theme: row.theme,
    tags: row.tags,
    supplement: row.supplement,
    mechanics: row.mechanics,
  });
  const input: StartForumFlowInput = {
    guildId,
    threadId,
    theme: row.theme,
    flow: row.flow as FlowKind,
    tags: row.tags,
  };
  // 復元時は集めた情報サンプルは持たない (件数表示のみ最小)。
  const info: PaperReviewInfo = { voiceCount: 0, countCapped: false, samples: [] };
  const pending: PendingPaperReview = { input, draft, info, hooks };
  paperReviewByThread.set(threadId, pending);
  return pending;
}

/** レビュー中ペーパーを 1 手前に戻す (Web の ↶戻すと対応)。 */
async function handlePaperReviewRevert(
  threadId: string,
  pending: PendingPaperReview,
  deps: FlowDiscordDeps
): Promise<void> {
  if (!canRevert(threadId)) {
    await postThreadNotice(deps, threadId, "↶ これ以上戻せません。");
    return;
  }
  const reverted = revertLast(threadId);
  if (!reverted) {
    await postThreadNotice(deps, threadId, "↶ これ以上戻せません。");
    return;
  }
  pending.draft = withDerivedStructure(reverted.bodyMd, pending.draft);
  pending.input = { ...pending.input, theme: pending.draft.theme, tags: pending.draft.tags };
  persistDiscordDraft(pending.input, pending.draft);
  paperReviewByThread.set(threadId, pending);
  scheduleReviewAutoStart(threadId, deps);
  await postThreadNotice(deps, threadId, `↶ ${reverted.changeSummary || "1 手前に戻しました"}`);
  await postThreadNotice(deps, threadId, renderPaperReview(pending.draft, pending.info));
}

export async function handlePaperReviewReply(
  threadId: string,
  guildId: string,
  text: string,
  deps: FlowDiscordDeps,
  hooks?: FlowLiveHooks
): Promise<boolean> {
  // メモリに無くても永続ドラフト (status='draft') があれば再開する (再起動/別プロセス跨ぎ)。
  const pending = paperReviewByThread.get(threadId) ?? rehydratePaperReview(threadId, guildId, hooks);
  if (!pending) return false;
  const trimmed = text.trim();
  if (!trimmed) return true; // 空返信は無視 (レビューは継続)

  try {
    // 承認 → 確定ペーパーで議論開始。
    if (isApprovalText(trimmed)) {
      await handlePaperReviewApproval(threadId, deps, hooks);
      return true;
    }

    // 戻す → 1 手前の本文に戻す (Web の ↶戻すと対応)。
    if (isRevertText(trimmed)) {
      await handlePaperReviewRevert(threadId, pending, deps);
      return true;
    }

    // 調整 → LLM でペーパーに反映し、更新版を再掲。
    const edited = await applyPaperEdit(pending.draft, trimmed, deps.llm, {
      model: getConfig().flow.paperReview.model || undefined,
      warn: (m) => console.warn(`  [paper-review ${threadId}] ${m}`),
    });
    pending.draft = edited.draft;
    // 議題/タグの編集は dispatch 引数に反映する (override はメカニクス/観点補足のみ運ぶ)。
    pending.input = { ...pending.input, theme: edited.draft.theme, tags: edited.draft.tags };
    paperReviewByThread.set(threadId, pending);
    scheduleReviewAutoStart(threadId, deps); // 調整があったら自動開始を延長

    await postThreadNotice(deps, threadId, `📝 ${edited.changeSummary}`);
    if (edited.applied) {
      // 版履歴に追記 (戻すの基点) + 一覧の下書き行を最新内容に同期。
      appendRevision({ sessionId: threadId, bodyMd: edited.draft.bodyMd, changeSummary: edited.changeSummary, origin: "llm-edit" });
      persistDiscordDraft(pending.input, pending.draft);
      await postThreadNotice(deps, threadId, renderPaperReview(pending.draft, pending.info));
      await postThreadNotice(deps, threadId, "他に調整があれば返信、よければ **「開始」** と返信 (または ✅)、1 手戻すなら **「戻す」** と返信してください。");
    }
  } catch (err) {
    console.warn(`  flow-live: ペーパーレビュー返信処理失敗 (thread=${threadId}): ${(err as Error).message}`);
    await postThreadNotice(deps, threadId, `⚠️ 調整の反映に失敗しました: ${(err as Error).message}`);
  }
  return true;
}

/**
 * レビュー中ペーパーを承認して議論を開始する (テキスト「開始」/ ✅ リアクション 共通)。
 * @returns 承認待ちがあって処理したら true。
 */
export async function handlePaperReviewApproval(
  threadId: string,
  deps: FlowDiscordDeps,
  hooks?: FlowLiveHooks,
  /** メモリに無い時の再開用 guildId (✅ リアクション経路から。scene 用)。 */
  guildId?: string
): Promise<boolean> {
  const pending =
    paperReviewByThread.get(threadId) ?? rehydratePaperReview(threadId, guildId ?? "dm", hooks);
  if (!pending) return false;
  if (pending.timer) clearTimeout(pending.timer);
  paperReviewByThread.delete(threadId);
  await postThreadNotice(deps, threadId, "✅ ペーパーを承認しました。議論を始めます…");
  recordForcedDebatability(threadId, pending.info);
  await runDiscussionDispatch(pending.input, deps, pending.hooks ?? hooks, overrideFromPending(pending));
  return true;
}

/**
 * レビュー待ちを破棄する (フロー再提案で人間が別タイプを選び直したとき用)。
 * タイマー解除 + メモリから削除に加え、永続 draft 行も破棄する
 * (残すと hasPaperReview → rehydrate で復活し、新フローのスレッド返信を横取りするため)。
 * @returns 何かを破棄したら true。
 */
export function cancelPaperReview(threadId: string): boolean {
  const pending = paperReviewByThread.get(threadId);
  if (pending?.timer) clearTimeout(pending.timer);
  const hadMemory = paperReviewByThread.delete(threadId);
  let hadDraft = false;
  try {
    // draft 状態の行だけ消す (started 済みの議論データは触らない)。
    if (getDraftPaper(threadId) !== null) hadDraft = deleteFlowSession(threadId);
  } catch (e) {
    console.warn(`  [paper-review ${threadId}] レビュー破棄で draft 削除失敗: ${(e as Error).message}`);
  }
  return hadMemory || hadDraft;
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
