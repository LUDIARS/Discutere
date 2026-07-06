/**
 * FlowDirector — 議論フロー (discussion.md) の進行オーケストレータ。
 *
 * persona-engine を使わず、ターン駆動の同期ループで議論を回す。
 * runFlow は「進行の台本」(ループ制御・評価・サマリ・収束判定・結論の呼び出し順) のみを持ち、
 * 各 step の実体は flow-setup / persona-setup / facilitator-turn / persona-turn に分割 (respec 10)。
 * すべての LLM 呼び出しは withCostLog 経由で llm_call_log に記録される。
 * エラーは握り潰さず発話に出す (OVERVIEW §10)。
 */

import { randomUUID } from "node:crypto";
import type { LLMClient } from "../persona-engine/llm/client.js";
import type { FlowTag } from "./tags.js";
import { getConfig } from "../config.js";
import type { FlowPersona, Rng } from "./personas.js";
import { buildFlowCast } from "./persona-cast.js";
import { setupSessionPersonas } from "./persona-setup.js";
import { selectPossessionByTheme, describePossession } from "./persona-pool.js";
import { createVoiceCache } from "./voice-cache.js";
import {
  updatePaperBody,
  synthesizeOpinions,
  type ContextVoice,
  type DiscussionPaper,
  type RoundSummary,
} from "./discussion-paper.js";
import { renderProgressMarkdown } from "./paper-markdown.js";
import type { YoutubeSearchFn, MechanicSummary } from "./investigate.js";
import { setupFlowPaper, type PaperOverride } from "./flow-setup.js";
import { runFacilitatorTurn, type PreviousRoundContext } from "./facilitator-turn.js";
import { runPersonaTurn } from "./persona-turn.js";
import { buildTurnOrder } from "./turn-scheduler.js";
import { getFlowDb } from "./db/connection.js";
import { runRoundVote, type VoteResult } from "./vote.js";
import { summarizeRound } from "./round-summary.js";
import { generateConclusion } from "./conclusion.js";

export type { PaperOverride } from "./flow-setup.js";

export interface FlowUtteranceRecord {
  id: string;
  sessionId: string;
  paperId: string;
  round: number;
  turn: number;
  personaId: string;
  personaName: string;
  role: string;
  stance: string;
  text: string;
  isError: boolean;
  /** その発話で演じていた憑依ペルソナの露出名 (B, item4)。憑依なしは undefined。 */
  possessionName?: string;
  /**
   * 会話行為 (respec 05 / PR-B, dialectic エンジン専用)。
   * claim/support/rebut/concede/question/synthesize。rounds エンジンの発話は undefined
   * (DB では NULL のまま。旧行の読み出しは claim 扱い)。
   */
  act?: string;
  /** 応答先 utterance id (act とセット。claim/question は undefined 可)。 */
  targetId?: string;
}

/**
 * ラウンド投票結果の通知 (item3)。Discord/Slack はこれを受けて
 * 各意見メッセージに「投票」リアクションを付け、得票集計を可視化する。
 */
export interface VoteEvent {
  round: number;
  /**
   * 集計の意味種別 (respec 03, A10)。"votes"=票数 / "scores"=improvement の機械スコア。
   * 表示層は scores のとき「n 票」表記をしない。
   */
  kind: "votes" | "scores";
  /** utterance_id ごとの得票数 (kind="scores" のときは射影スコア)。 */
  tally: Record<string, number>;
  /** 最多得票の utterance_id (世論)。null = 無投票。 */
  winner: string | null;
  /** 投票対象だった意見 (id → 発話主体名)。表示に使う。 */
  candidates: Array<{ id: string; personaName: string }>;
}

export interface FlowDirectorDeps {
  /** LLM クライアント (各ターンで withCostLog でラップして使う) */
  llm: LLMClient;
  /** ユーザ意見取得 (optional。Kuzu が無い環境や test では空配列を返すスタブを渡す) */
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  /** YouTube 検索 (optional。未設定なら YouTube 補完は走らない) */
  youtubeSearch?: YoutubeSearchFn;
  /** 発話後のコールバック (Discord/WebUI への投稿等) */
  onUtterance?: (u: FlowUtteranceRecord) => void | Promise<void>;
  /** ラウンド投票後のコールバック (item3: リアクションで得票を可視化する)。 */
  onVote?: (e: VoteEvent) => void | Promise<void>;
  /** ログ・警告出力 */
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
  rng?: Rng;
  /** `data/games/` ディレクトリパス (省略時は既定値) */
  gamesDir?: string;
  /** セッション ID を外部から指定する (WebUI が起動前に id を返してポーリングするため)。既定は randomUUID。 */
  sessionId?: string;
  /** このセッションのラウンド数 (省略時は config.flow.rounds)。1..MAX_ROUNDS にクランプ。 */
  rounds?: number;
  /** このセッションの 1 ラウンドあたりターン数 (省略時は config.flow.turnsPerRound)。1..MAX_TURNS にクランプ。 */
  turnsPerRound?: number;
  /** 議論/改善キャストに含める生成済みペルソナの id/name。 */
  personaIds?: string[];
  /**
   * 憑依 (B): テーマから嗜好を類推し、プール最近傍ペルソナを投稿主体の 1 枠 (opinion) に充てる。
   * 既定 true。プールが空 / 一致なしなら従来生成キャストのまま (no-op)。
   */
  possess?: boolean;
  /**
   * 確定済みディスカッションペーパー (人間レビューゲートで調整・承認したもの)。
   * 指定時は investigate (step 1) を **省略**し、このメカニクス/観点補足で議論を回す。
   * theme / tags は通常の引数で渡す (= 人間が直した値をそのまま渡す)。
   */
  paperOverride?: PaperOverride;
}

/** 都度指定ラウンド/ターン数の暴走ガード上限 (コスト保護)。 */
export const MAX_ROUNDS = 10;
export const MAX_TURNS_PER_ROUND = 20;

/** 都度指定値を [1, max] にクランプ。未指定/非有限なら fallback を返す。 */
export function clampCount(value: number | undefined, fallback: number, max: number): number {
  if (value == null || !Number.isFinite(value)) return fallback;
  return Math.max(1, Math.min(max, Math.floor(value)));
}

export interface FlowDirectorResult {
  sessionId: string;
  paperId: string;
  utterances: FlowUtteranceRecord[];
  rounds: number;
  conclusion: string;
  concluded: boolean;
}

/**
 * ラウンドの世論決定 (discussion.md step 6) を差し替えるための評価コンテキスト。
 * 議論フローは中立投票 (vote)、改善フローは機械スコア (design_gap) を注入する。
 */
export interface RoundEvalContext {
  theme: string;
  sessionId: string;
  paperId: string;
  round: number;
  mechanics: MechanicSummary[];
  tags: readonly FlowTag[];
  /** 投票/スコア候補 (ファシリテーター発話・エラー発話を除いた当ラウンドの意見) */
  candidates: Array<{ id: string; personaName: string; text: string }>;
  llm: LLMClient;
  warn: (msg: string) => void;
}

/**
 * ラウンド世論決定の戦略。返り値は VoteResult 形 ({ kind, tally, winner, winnerShare })。
 * winner = そのラウンドの主要意見 (世論) の utterance id。
 */
export type RoundEvaluator = (ctx: RoundEvalContext) => Promise<VoteResult>;

/** runFlow のオプション (議論フロー/改善フローで共有)。 */
export interface FlowRunOptions extends FlowDirectorDeps {
  /** フロー種別 (コストログ / ペーパーに記録)。既定 "discussion"。 */
  flow?: string;
  /** ラウンド世論決定の戦略。既定は中立投票 (runRoundVote)。 */
  evaluateRound?: RoundEvaluator;
}

function defaultRng(): number {
  return Math.random();
}

/** 既定のラウンド世論決定 = 中立投票 (discussion.md step 6, respec 03 のレンズ/シャッフル込み)。 */
function makeDefaultEvaluator(rng: Rng): RoundEvaluator {
  const cfg = getConfig();
  return (ctx) =>
    runRoundVote({
      theme: ctx.theme,
      sessionId: ctx.sessionId,
      round: ctx.round,
      voterCount: cfg.flow.voterCount,
      utterances: ctx.candidates,
      llm: ctx.llm,
      lenses: cfg.flow.voteLenses,
      rng,
      warn: ctx.warn,
    });
}

/**
 * 投票候補の判定。
 * ファシリテーターの発話 (議題提示・進行) は「意見」ではないため投票候補から除外する。
 * エラー発話・他ラウンドの発話も除く。
 */
export function isVoteCandidate(u: FlowUtteranceRecord, round: number): boolean {
  return u.round === round && !u.isError && u.role !== "facilitator";
}

/** flow_utterance テーブルにターン発話を永続化する。 */
export function persistUtterance(u: FlowUtteranceRecord): void {
  const db = getFlowDb();
  db.prepare(
    `INSERT INTO flow_utterance
       (id, session_id, paper_id, round, turn, persona_id, persona_name, role, stance, text, is_error, possession_name, act, target_id, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`
  ).run(
    u.id,
    u.sessionId,
    u.paperId,
    u.round,
    u.turn,
    u.personaId,
    u.personaName,
    u.role,
    u.stance,
    u.text,
    u.isError ? 1 : 0,
    u.possessionName ?? null,
    // 会話行為 (respec 05)。rounds エンジンは undefined → NULL (acts は dialectic 専用)。
    u.act ?? null,
    u.targetId ?? null,
    Date.now()
  );
}

/**
 * 憑依 (B, item4): テーマから嗜好を類推し、プール最近傍ペルソナを opinion 1 枠に「憑依」させる。
 * casual な生成ペルソナ名は保持したまま、データ由来の人物像になりきって発言させる。
 * プールが空 / 一致なしなら no-op (従来生成キャストのまま)。投稿主体の代理は 1 体のみ (Q3)。
 */
function applyThemePossession(
  personas: FlowPersona[],
  theme: string,
  log: (msg: string) => void,
  warn: (msg: string) => void
): void {
  try {
    const hit = selectPossessionByTheme(theme, 1)[0];
    if (hit) {
      const seatIdx = personas.findIndex((p) => p.role === "opinion");
      if (seatIdx >= 0) {
        personas[seatIdx] = {
          ...personas[seatIdx],
          possession: { label: hit.persona.name, descriptor: describePossession(hit.persona) },
        };
        log(
          `憑依: 「${personas[seatIdx].name}」が「${hit.persona.name}」の人物像を憑依 (cos=${hit.similarity.toFixed(3)})`
        );
      }
    }
  } catch (e) {
    warn(`憑依スキップ (${(e as Error).message})`);
  }
}

/**
 * ファシリテーター開幕に注入する前ラウンド文脈を組み立てる (respec 02, A8)。
 * round 1 (前ラウンドなし) は null。winner 発話は allUtterances から引く。
 */
function buildPreviousRoundContext(
  rounds: readonly RoundSummary[],
  evaluations: readonly VoteResult[],
  allUtterances: readonly FlowUtteranceRecord[]
): PreviousRoundContext | null {
  const prev = rounds[rounds.length - 1];
  if (!prev) return null;
  const lastEval = evaluations[evaluations.length - 1];
  const winnerRecord = lastEval?.winner ? allUtterances.find((u) => u.id === lastEval.winner) : undefined;
  return {
    round: prev.round,
    summary: prev.summary,
    aufhebung: prev.aufhebung,
    winner: winnerRecord ? { personaName: winnerRecord.personaName, text: winnerRecord.text } : null,
  };
}

/**
 * [6] 世論決定 + onVote 通知 (item3)。評価戦略を呼び、Discord/Slack へ kind 付きで通知する。
 * 通知失敗は warn のみ (議論は止めない)。
 */
async function evaluateAndNotify(args: {
  evaluateRound: RoundEvaluator;
  theme: string;
  sessionId: string;
  paperId: string;
  round: number;
  mechanics: MechanicSummary[];
  tags: readonly FlowTag[];
  /** 投票/スコア候補 (isVoteCandidate で絞った当ラウンドの意見レコード)。 */
  candidates: FlowUtteranceRecord[];
  llm: LLMClient;
  onVote?: (e: VoteEvent) => void | Promise<void>;
  log: (msg: string) => void;
  warn: (msg: string) => void;
}): Promise<VoteResult> {
  const { evaluateRound, theme, sessionId, paperId, round, mechanics, tags, candidates, llm, onVote, log, warn } = args;
  const evaluation = await evaluateRound({
    theme,
    sessionId,
    paperId,
    round,
    mechanics,
    tags,
    candidates: candidates.map((u) => ({ id: u.id, personaName: u.personaName, text: u.text })),
    llm,
    warn,
  });
  if (evaluation.winner) {
    log(`ラウンド ${round} 世論: ${evaluation.winner}`);
  }
  if (onVote) {
    try {
      await onVote({
        round,
        kind: evaluation.kind ?? "votes",
        tally: evaluation.tally,
        winner: evaluation.winner,
        candidates: candidates.map((u) => ({ id: u.id, personaName: u.personaName })),
      });
    } catch (e) {
      warn(`ラウンド ${round} onVote 通知失敗: ${(e as Error).message}`);
    }
  }
  return evaluation;
}

/**
 * [3] セッションキャストの組み立て: 生成 (stance 固定) → 憑依 → 価値軸/核主張 + 永続。
 */
async function assembleCast(args: {
  theme: string;
  sessionId: string;
  flow: string;
  llm: LLMClient;
  isLocal: boolean;
  rng: Rng;
  possess: boolean;
  personaIds?: readonly string[];
  log: (msg: string) => void;
  warn: (msg: string) => void;
}): Promise<FlowPersona[]> {
  const cfg = getConfig();
  const { theme, sessionId, flow, llm, isLocal, rng, possess, personaIds, log, warn } = args;
  const defaultModel = cfg.llm.model ?? "claude-haiku-4-5-20251001";
  const cast = buildFlowCast({
    count: cfg.flow.personaCount,
    defaultModel,
    isLocal,
    rng,
    personaIds,
    warn,
  });
  if (possess && !(personaIds && personaIds.length > 0)) {
    applyThemePossession(cast, theme, log, warn);
  }
  // 価値軸 + 核主張の一括生成 (セッション 1 回の LLM 呼び出し) + flow_session_persona 永続。
  // LLM 失敗は valueAxis なしで degrade (persona-setup 内で warn)。
  const personas = await setupSessionPersonas({
    theme,
    sessionId,
    personas: cast,
    llm,
    flow,
    model: cfg.flow.personaSetupModel || undefined,
    warn,
  });
  log(`ペルソナ ${personas.length} 人生成: ${personas.map((p) => `${p.name}(${p.role}/${p.stance})`).join(", ")}`);
  return personas;
}

/**
 * [7] ラウンドサマリ + 止揚を記録し、ペーパー本文 (body_md) をライブ更新する。
 * 新しい止揚は allAufhebung / paper.rounds に反映される (呼び出し側の状態を進める)。
 */
async function summarizeAndRecord(args: {
  theme: string;
  sessionId: string;
  paperId: string;
  round: number;
  roundUtterances: Array<{ personaName: string; text: string }>;
  allAufhebung: string[];
  paper: DiscussionPaper;
  flow: string;
  llm: LLMClient;
  log: (msg: string) => void;
  warn: (msg: string) => void;
}): Promise<void> {
  const { theme, sessionId, paperId, round, roundUtterances, allAufhebung, paper, flow, llm, log, warn } = args;
  const summaryResult = await summarizeRound({
    theme,
    sessionId,
    paperId,
    round,
    utterances: roundUtterances,
    stockedAufhebung: [...allAufhebung],
    llm,
    warn,
    flow,
  });
  allAufhebung.push(...summaryResult.newAufhebung);

  const roundSummary: RoundSummary = {
    round,
    summary: summaryResult.summary,
    aufhebung: summaryResult.newAufhebung,
  };
  paper.rounds.push(roundSummary);

  // ペーパー更新 (ライブ): base ブリーフ + ここまでの議論の経過 (まとめ/止揚) を焼き直して
  // discussion_paper.body_md に上書き → Web UI で「ペーパーが更新されていく」。
  try {
    updatePaperBody(paperId, renderProgressMarkdown(paper.bodyMd ?? "", paper.rounds));
  } catch (e) {
    warn(`ペーパー更新失敗 (議論は続行): ${(e as Error).message}`);
  }

  log(`ラウンド ${round} 終了 (${roundUtterances.length} 発話, 止揚: ${summaryResult.newAufhebung.length})`);
}

/**
 * 早期収束の判定 (respec 04, A6 の暫定緩和)。
 * 止揚 ≥ aufhebungTarget **かつ** 直近ラウンドの投票集中度 winnerShare ≥ flow.convergeShare の AND。
 * improvement (kind="scores") は winnerShare を計算できないため従来条件 (止揚のみ) のまま。
 */
function shouldConvergeEarly(
  aufhebungCount: number,
  lastEvaluation: VoteResult | undefined,
  log: (msg: string) => void
): boolean {
  const cfg = getConfig();
  if (aufhebungCount < cfg.facilitator.aufhebungTarget) return false;
  if (lastEvaluation?.kind === "scores") {
    log(`止揚 ${aufhebungCount} 件が上限 (${cfg.facilitator.aufhebungTarget}) に達したため早期収束`);
    return true;
  }
  const share = lastEvaluation?.winnerShare ?? 0;
  if (share >= cfg.flow.convergeShare) {
    log(
      `止揚 ${aufhebungCount} 件 + 投票集中度 ${share.toFixed(2)} ≥ ${cfg.flow.convergeShare} のため早期収束`
    );
    return true;
  }
  log(
    `止揚 ${aufhebungCount} 件は上限に達したが、投票集中度 ${share.toFixed(2)} < ${cfg.flow.convergeShare} (世論が割れている) — 続行`
  );
  return false;
}

/**
 * 議論後のペーパー確定 (LLM リファイン opt-in → 最終 body_md 焼き込み)。
 * 議論ループの後に走るためペルソナ system のキャッシュには影響しない。失敗は元の base で degrade。
 */
async function finalizePaperBody(args: {
  paper: DiscussionPaper;
  theme: string;
  tags: readonly FlowTag[];
  supplement: string;
  mechanics: MechanicSummary[];
  conclusionSummary: string;
  sessionId: string;
  paperId: string;
  flow: string;
  llm: LLMClient;
  log: (msg: string) => void;
  warn: (msg: string) => void;
}): Promise<void> {
  const cfg = getConfig();
  const { paper, theme, tags, supplement, mechanics, conclusionSummary, sessionId, paperId, flow, llm, log, warn } = args;

  let finalBase = paper.bodyMd ?? "";
  if (cfg.flow.paperRefine.enabled) {
    try {
      const { refinePaperBrief } = await import("./paper-refine.js");
      const { stripProgress, markdownToPaperDraft } = await import("./paper-markdown.js");
      const refined = await refinePaperBrief({
        baseBodyMd: stripProgress(finalBase),
        theme,
        rounds: paper.rounds,
        conclusion: conclusionSummary,
        llm,
        sessionId,
        flow,
        model: cfg.flow.paperRefine.model || undefined,
        warn,
      });
      if (refined && refined.trim()) {
        finalBase = refined;
        // 構造化列 (観点補足 / メカニクス) を派生し直して追従させる (theme/tags は据え置き)。
        const derived = markdownToPaperDraft(refined, { theme, tags: [...tags], supplement, mechanics });
        (await import("./discussion-paper.js")).updatePaperDerived(paperId, {
          supplement: derived.supplement,
          mechanics: derived.mechanics,
        });
        log(`ペーパーリファイン完了 (メカニクス ${derived.mechanics.length} 件)`);
      }
    } catch (e) {
      warn(`ペーパーリファインスキップ (議論は続行): ${(e as Error).message}`);
    }
  }

  // ペーパー最終更新 (ライブ): 議論の経過 + 結論まで焼き込む (リファイン済みなら refined base)。
  try {
    updatePaperBody(paperId, renderProgressMarkdown(finalBase, paper.rounds, conclusionSummary));
  } catch (e) {
    warn(`ペーパー最終更新失敗: ${(e as Error).message}`);
  }
}

/**
 * 議論骨格フローを実行する (discussion.md の 9 ステップ)。
 * step 6 の世論決定は options.evaluateRound で差し替え可能 (議論=投票 / 改善=機械スコア)。
 *
 * @param theme テーマ文字列
 * @param tags フロータグ (機密/内部/運用/開発 等)
 * @param options 依存注入 + flow 種別 + 世論決定戦略
 */
export async function runFlow(
  theme: string,
  tags: readonly FlowTag[],
  options: FlowRunOptions
): Promise<FlowDirectorResult> {
  const cfg = getConfig();
  const {
    llm,
    listExternalVoices = () => [],
    youtubeSearch,
    onUtterance,
    log = (m) => console.log(`[flow] ${m}`),
    warn = (m) => console.warn(`[flow/warn] ${m}`),
    rng = defaultRng,
    gamesDir,
    flow = "discussion",
  } = options;

  // 発話時の DB 参照キャッシュ (item5): 同一テーマ/語の外部の声 lookup を
  // セッション内で 1 回に集約し、全ペルソナのターンで使い回す。
  const voiceCache = createVoiceCache(listExternalVoices);

  // ラウンド世論決定の戦略。既定は中立投票 (discussion.md step 6)。
  // 改善フローは机上の機械スコア (design_gap) を注入する (improvement.md)。
  const evaluateRound: RoundEvaluator = options.evaluateRound ?? makeDefaultEvaluator(rng);

  const sessionId = options.sessionId ?? randomUUID();
  const isLocal = cfg.llm.backend === "local";
  // ラウンド/ターン数は議論ごとの指定 (options) を優先し、無ければ config 既定。暴走ガードでクランプ。
  const rounds = clampCount(options.rounds, cfg.flow.rounds, MAX_ROUNDS);
  const turnsPerRound = clampCount(options.turnsPerRound, cfg.flow.turnsPerRound, MAX_TURNS_PER_ROUND);

  // ── [1][2] 調査 + ディスカッションペーパー初期化 (flow-setup) ─────────────
  const { paper, paperId, investigation } = await setupFlowPaper({
    theme,
    tags,
    flow,
    sessionId,
    llm,
    voiceCache,
    youtubeSearch,
    gamesDir,
    paperOverride: options.paperOverride,
    log,
    warn,
  });
  const { mechanics, supplement } = paper;

  // 各 step 関数へ渡す共通コンテキスト (進行の台本を宣言的に保つ)。
  const base = { theme, sessionId, paperId, flow, llm, log, warn } as const;

  // ── [3] ペルソナ生成 (stance セッション固定 + 憑依 + 価値軸/核主張) ─────────
  const personas = await assembleCast({
    ...base,
    isLocal,
    rng,
    possess: options.possess !== false,
    personaIds: options.personaIds,
  });

  // synthetic opinions (機密タグ用)
  const syntheticOpinions = tags.includes("機密") ? synthesizeOpinions(mechanics) : [];

  const allUtterances: FlowUtteranceRecord[] = [];
  const allAufhebung: string[] = [];
  const roundEvaluations: VoteResult[] = [];

  /** 発話レコードの永続 + 通知 (persistUtterance → onUtterance の順序を全ターンで維持)。 */
  const commitUtterance = async (
    record: FlowUtteranceRecord,
    roundUtterances: Array<{ personaName: string; text: string }>
  ): Promise<void> => {
    persistUtterance(record);
    roundUtterances.push({ personaName: record.personaName, text: record.text });
    allUtterances.push(record);
    if (onUtterance) await onUtterance(record);
  };

  // ── ラウンドループ ────────────────────────────────────────────────────────
  const facilitatorPersona = personas.find((p) => p.role === "facilitator") ?? personas[0];

  // 承認済み論点 (09): ペーパーゲートで人間が確認した論点を開幕プロンプトの参考に注入する。
  const override = options.paperOverride;
  const guidingIssues = (override?.issues ?? []).map((s) => s.trim()).filter(Boolean).slice(0, 5);

  for (let round = 1; round <= rounds; round++) {
    log(`ラウンド ${round}/${rounds} 開始`);
    const roundUtterances: Array<{ personaName: string; text: string }> = [];

    // ── [4] ファシリテーター開幕ターン (議題提示 + 前ラウンド文脈 + 承認済み論点) ──
    const facilitatorRecord = await runFacilitatorTurn({
      ...base,
      round,
      facilitator: facilitatorPersona,
      previousRound: buildPreviousRoundContext(paper.rounds, roundEvaluations, allUtterances),
      guidingIssues,
    });
    if (facilitatorRecord) await commitUtterance(facilitatorRecord, roundUtterances);

    // ── [5] ターンループ (シャッフル輪番。facilitator は開幕専任で対象外) ──
    const turnOrder = buildTurnOrder(personas, turnsPerRound, rng);
    for (let turn = 1; turn <= turnsPerRound; turn++) {
      const persona = turnOrder[turn - 1];
      if (!persona) break; // 発言者ゼロ (personaCount=1 等)
      const record = await runPersonaTurn({
        ...base,
        persona,
        round,
        turn,
        paper,
        roundUtterances,
        voiceCache,
        investigation,
        syntheticOpinions,
      });
      if (record) await commitUtterance(record, roundUtterances);
    }

    // ── [6] 世論決定 (議論=中立投票 / 改善=機械スコア) + onVote 通知 ─────────
    const evaluation = await evaluateAndNotify({
      ...base,
      evaluateRound,
      round,
      mechanics,
      tags,
      candidates: allUtterances.filter((u) => isVoteCandidate(u, round)),
      onVote: options.onVote,
    });
    roundEvaluations.push(evaluation);

    // ── [7] ラウンドサマリ + 止揚 + ペーパーライブ更新 ─────────────────────
    await summarizeAndRecord({ ...base, round, roundUtterances, allAufhebung, paper });

    // ── [9] 早期収束チェック: 止揚到達 AND 投票集中度 (respec 04) ────────────
    if (shouldConvergeEarly(allAufhebung.length, evaluation, log)) break;
  }

  // ── [9] 結論生成 (全ラウンドサマリを入力に含める, respec 04) ────────────────
  log(`発話時 RAG: lookup ${voiceCache.misses()} 回 / キャッシュ再利用 ${voiceCache.hits()} 回 (item5)`);
  log("結論生成中...");
  const conclusionResult = await generateConclusion({
    ...base,
    allUtterances,
    allAufhebung,
    rounds: paper.rounds,
    voteResults: roundEvaluations,
    // 議論適性: 低のまま強行した議論は、結論にその旨を併記する (09)。
    annotation:
      override?.debatability && !override.debatability.debatable
        ? `議論適性: 低 (両論武装可能な争点 ${override.debatability.armableBothCount} 件)`
        : undefined,
  });
  log(`結論: ${conclusionResult.concluded ? conclusionResult.summary.slice(0, 80) : "結論なし"}`);

  // ── ペーパー本文の確定 (LLM リファイン opt-in + 最終焼き込み) ───────────────
  await finalizePaperBody({
    ...base,
    paper,
    tags,
    supplement,
    mechanics,
    conclusionSummary: conclusionResult.summary,
  });

  return {
    sessionId,
    paperId,
    utterances: allUtterances,
    rounds: paper.rounds.length,
    conclusion: conclusionResult.summary,
    concluded: conclusionResult.concluded,
  };
}

/**
 * 議論フローを実行する (discussion.md)。
 * 世論決定 = 中立投票 (step 6)。runFlow の薄いラッパ。
 *
 * @param theme テーマ文字列
 * @param tags フロータグ (機密/内部/運用/開発 等)
 * @param deps 依存注入
 */
export async function runDiscussionFlow(
  theme: string,
  tags: readonly FlowTag[],
  deps: FlowDirectorDeps
): Promise<FlowDirectorResult> {
  return runFlow(theme, tags, { ...deps, flow: "discussion" });
}
