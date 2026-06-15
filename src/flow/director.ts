/**
 * FlowDirector — 議論フロー (discussion.md) の進行オーケストレータ。
 *
 * persona-engine を使わず、ターン駆動の同期ループで議論を回す。
 * すべての LLM 呼び出しは withCostLog 経由で llm_call_log に記録される。
 * エラーは握り潰さず発話に出す (OVERVIEW §10)。
 */

import { randomUUID } from "node:crypto";
import type { LLMClient } from "../persona-engine/llm/client.js";
import type { FlowTag } from "./tags.js";
import { getConfig } from "../config.js";
import { withCostLog } from "./cost-logger.js";
import { generateFlowPersonas, pickRandomPersona, decideStance, type FlowPersona, type Rng } from "./personas.js";
import { selectPossessionByTheme, toFlowPersona } from "./persona-pool.js";
import {
  buildPersonaPaper,
  paperToPrompt,
  persistPaper,
  synthesizeOpinions,
  type ContextVoice,
  type DiscussionPaper,
  type RoundSummary,
} from "./discussion-paper.js";
import { investigateTheme, type YoutubeSearchFn, type MechanicSummary } from "./investigate.js";
import { getFlowDb } from "./db/connection.js";
import { runRoundVote, type VoteResult } from "./vote.js";
import { summarizeRound } from "./round-summary.js";
import { generateConclusion } from "./conclusion.js";

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
  /**
   * 憑依 (B): テーマから嗜好を類推し、プール最近傍ペルソナを投稿主体の 1 枠 (opinion) に充てる。
   * 既定 true。プールが空 / 一致なしなら従来生成キャストのまま (no-op)。
   */
  possess?: boolean;
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
 * ラウンド世論決定の戦略。返り値は VoteResult 形 ({ tally, winner })。
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
       (id, session_id, paper_id, round, turn, persona_id, persona_name, role, stance, text, is_error, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`
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
    Date.now()
  );
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

  // ラウンド世論決定の戦略。既定は中立投票 (discussion.md step 6)。
  // 改善フローは机上の機械スコア (design_gap) を注入する (improvement.md)。
  const evaluateRound: RoundEvaluator =
    options.evaluateRound ??
    ((ctx) =>
      runRoundVote({
        theme: ctx.theme,
        sessionId: ctx.sessionId,
        round: ctx.round,
        voterCount: cfg.flow.voterCount,
        utterances: ctx.candidates,
        llm: ctx.llm,
        warn: ctx.warn,
      }));

  const sessionId = options.sessionId ?? randomUUID();
  const isLocal = cfg.llm.backend === "local";
  // ラウンド/ターン数は議論ごとの指定 (options) を優先し、無ければ config 既定。暴走ガードでクランプ。
  const rounds = clampCount(options.rounds, cfg.flow.rounds, MAX_ROUNDS);
  const turnsPerRound = clampCount(options.turnsPerRound, cfg.flow.turnsPerRound, MAX_TURNS_PER_ROUND);

  // ── [1] 調査 ──────────────────────────────────────────────────────────────
  log(`調査開始: "${theme}" (タグ: [${tags.join(", ")}])`);
  const investigation = await investigateTheme({
    theme,
    tags,
    gamesDir,
    youtubeSearch,
    youtubeMaxComments: cfg.flow.youtubeMaxComments,
    warn,
  });
  const mechanics: MechanicSummary[] = investigation.mechanics;

  // ── [2] ディスカッションペーパー初期化 ─────────────────────────────────
  const supplement = (await import("./tags.js")).paperSupplement(tags);
  const paperId = persistPaper({ sessionId, theme, tags: [...tags], mechanics, supplement }, flow);
  const paper: DiscussionPaper = {
    paperId,
    sessionId,
    theme,
    tags: [...tags],
    mechanics,
    supplement,
    rounds: [],
  };

  // ── [3] ペルソナ生成 ────────────────────────────────────────────────────
  const defaultModel = cfg.llm.model ?? "claude-haiku-4-5-20251001";
  const personas: FlowPersona[] = generateFlowPersonas({
    count: cfg.flow.personaCount,
    defaultModel,
    isLocal,
    rng,
  });

  // ── 憑依 (B): テーマから嗜好を類推し、プール最近傍ペルソナを opinion 1 枠に充てる ──
  // プールが空 / 一致なしなら no-op (従来生成キャストのまま)。投稿主体の代理は 1 体のみ (Q3)。
  if (options.possess !== false) {
    try {
      const hit = selectPossessionByTheme(theme, 1)[0];
      if (hit) {
        const seatIdx = personas.findIndex((p) => p.role === "opinion");
        if (seatIdx >= 0) {
          personas[seatIdx] = toFlowPersona(hit.persona, { role: "opinion", defaultModel, isLocal });
          log(`憑依: 「${hit.persona.name}」をテーマ嗜好で投稿主体枠にアサイン (cos=${hit.similarity.toFixed(3)})`);
        }
      }
    } catch (e) {
      warn(`憑依スキップ (${(e as Error).message})`);
    }
  }

  log(`ペルソナ ${personas.length} 人生成: ${personas.map((p) => `${p.name}(${p.role})`).join(", ")}`);

  // synthetic opinions (機密タグ用)
  const syntheticOpinions = tags.includes("機密") ? synthesizeOpinions(mechanics) : [];

  const allUtterances: FlowUtteranceRecord[] = [];
  const allAufhebung: string[] = [];
  const roundEvaluations: VoteResult[] = [];

  // ── ラウンドループ ────────────────────────────────────────────────────────
  const facilitatorPersona = personas.find((p) => p.role === "facilitator") ?? personas[0];

  for (let round = 1; round <= rounds; round++) {
    log(`ラウンド ${round}/${rounds} 開始`);
    const roundUtterances: Array<{ personaName: string; text: string }> = [];

    // ── [4] ファシリテーター開幕ターン (議題提示) ─────────────────────────
    {
      const facilitatorPrompt =
        `あなたは議論の進行役です。\n` +
        `テーマ「${theme}」について、ラウンド ${round} の議論を始めてください。\n` +
        `参加者が意見を出しやすいよう、1〜2 文で議題を提示してください。`;

      const facilitatorLogged = withCostLog(llm, {
        flow,
        sessionId,
        round,
        turn: 0,
        role: facilitatorPersona.role,
        persona: facilitatorPersona.name,
        location: "facilitator",
      });

      const facilitatorResult = await facilitatorLogged.invoke({
        prompt: facilitatorPrompt,
        model: facilitatorPersona.model,
      });

      let facilitatorText: string;
      let isFacilitatorError = false;

      if (!facilitatorResult.ok) {
        facilitatorText = `[エラー: ${facilitatorResult.error}]`;
        isFacilitatorError = true;
        warn(`ラウンド ${round} ファシリテーター開幕ターン エラー: ${facilitatorResult.error}`);
      } else {
        facilitatorText = facilitatorResult.text.trim();
      }

      if (facilitatorText) {
        const facilitatorRecord: FlowUtteranceRecord = {
          id: randomUUID(),
          sessionId,
          paperId,
          round,
          turn: 0,
          personaId: facilitatorPersona.id,
          personaName: facilitatorPersona.name,
          role: facilitatorPersona.role,
          stance: "neutral",
          text: facilitatorText,
          isError: isFacilitatorError,
        };
        persistUtterance(facilitatorRecord);
        roundUtterances.push({ personaName: facilitatorPersona.name, text: facilitatorText });
        allUtterances.push(facilitatorRecord);
        if (onUtterance) await onUtterance(facilitatorRecord);
      }
    }

    // ── ターンループ ─────────────────────────────────────────────────────
    for (let turn = 1; turn <= turnsPerRound; turn++) {
      const persona = pickRandomPersona(personas, rng);
      const stance = decideStance(persona, rng);

      // ユーザ意見 RAG
      const userVoices: ContextVoice[] = listExternalVoices([theme], 5);

      // YouTube コメントを userVoices に追加 (補完時のみ)
      if (investigation.youtubeUsed && investigation.youtubeComments.length > 0) {
        const ytVoices: ContextVoice[] = investigation.youtubeComments
          .slice(0, 3)
          .map((c) => ({ content: c, source: "youtube" }));
        userVoices.push(...ytVoices);
      }

      // ペーパー組み立て
      const personaPaper = buildPersonaPaper({
        paper,
        persona,
        stance,
        currentRoundUtterances: roundUtterances,
        userVoices,
        syntheticOpinions,
      });
      const prompt = paperToPrompt(personaPaper, stance, persona);

      // LLM 呼び出し (コストログ付き)
      const logged = withCostLog(llm, {
        flow,
        sessionId,
        round,
        turn,
        role: persona.role,
        persona: persona.name,
        location: persona.role === "facilitator" ? "facilitator" : "utterance",
      });

      let utteranceText: string;
      let isError = false;

      const result = await logged.invoke({
        prompt,
        model: persona.model,
      });

      if (!result.ok) {
        utteranceText = `[エラー: ${result.error}]`;
        isError = true;
        warn(`ターン ${round}-${turn} (${persona.name}) LLM エラー: ${result.error}`);
      } else {
        utteranceText = result.text.trim();
        // 空応答は発話に数えない
        if (!utteranceText) {
          log(`ターン ${round}-${turn} (${persona.name}): 空応答 (スキップ)`);
          continue;
        }
      }

      const record: FlowUtteranceRecord = {
        id: randomUUID(),
        sessionId,
        paperId,
        round,
        turn,
        personaId: persona.id,
        personaName: persona.name,
        role: persona.role,
        stance,
        text: utteranceText,
        isError,
      };

      persistUtterance(record);
      roundUtterances.push({ personaName: persona.name, text: utteranceText });
      allUtterances.push(record);

      if (onUtterance) await onUtterance(record);
    }

    // ── [6] 世論決定 (議論=中立投票 / 改善=機械スコア) ──────────────────────
    const roundUtteranceRecords = allUtterances.filter((u) => isVoteCandidate(u, round));
    const evaluation = await evaluateRound({
      theme,
      sessionId,
      paperId,
      round,
      mechanics,
      tags,
      candidates: roundUtteranceRecords.map((u) => ({ id: u.id, personaName: u.personaName, text: u.text })),
      llm,
      warn,
    });
    roundEvaluations.push(evaluation);
    if (evaluation.winner) {
      log(`ラウンド ${round} 世論: ${evaluation.winner}`);
    }

    // ── [7] ラウンドサマリ + 止揚 ─────────────────────────────────────────
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

    log(`ラウンド ${round} 終了 (${roundUtterances.length} 発話, 止揚: ${summaryResult.newAufhebung.length})`);

    // ── [9] 早期収束チェック: 止揚が facilitator.aufhebungTarget に達したら打ち切り ──
    if (allAufhebung.length >= cfg.facilitator.aufhebungTarget) {
      log(`止揚 ${allAufhebung.length} 件が上限 (${cfg.facilitator.aufhebungTarget}) に達したため早期収束`);
      break;
    }
  }

  // ── [9] 結論生成 ──────────────────────────────────────────────────────────
  log("結論生成中...");
  const conclusionResult = await generateConclusion({
    theme,
    sessionId,
    paperId,
    allUtterances,
    allAufhebung,
    voteResults: roundEvaluations,
    llm,
    warn,
    flow,
  });
  log(`結論: ${conclusionResult.concluded ? conclusionResult.summary.slice(0, 80) : "結論なし"}`);

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
