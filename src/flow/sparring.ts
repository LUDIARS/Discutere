/**
 * 壁打ちフロー (sparring.md / OVERVIEW §9)。
 *
 * ユーザターン起点の対話フロー。ユーザの不定期発話に対し議論側が応答する。
 * ラウンド/ターンの固定制限なし。flow.sparringMaxTurns を暴走ガード上限とする。
 * 「まとめて」でラウンド締め (続行可)、「おわり」でフロー終了 (結論まとめ)。
 *
 * 入口は T7 dispatch が flow=壁打ち で本セッションを起動する (発話内容での自動判定はしない)。
 * 議論骨格は再実装せず T2/T3 の部品 (ディスカッションペーパー / ペルソナ / サマリ / 結論) を流用する。
 */

import { randomUUID } from "node:crypto";
import type { LLMClient } from "../persona-engine/llm/client.js";
import { getConfig } from "../config.js";
import type { FlowTag } from "./tags.js";
import { withCostLog } from "./cost-logger.js";
import {
  generateFlowPersonas,
  pickRandomPersona,
  decideStance,
  type FlowPersona,
  type Rng,
} from "./personas.js";
import {
  buildPersonaPaper,
  paperToPrompt,
  persistPaper,
  synthesizeOpinions,
  type ContextVoice,
  type DiscussionPaper,
} from "./discussion-paper.js";
import { investigateTheme, type YoutubeSearchFn } from "./investigate.js";
import { summarizeRound } from "./round-summary.js";
import { generateConclusion } from "./conclusion.js";
import { persistUtterance, type FlowUtteranceRecord } from "./director.js";
import { detectSparringCommand } from "./sparring-commands.js";
import { findPoolPersona, toFlowPersona } from "./persona-pool.js";

const FLOW = "sparring";

export interface SparringDeps {
  llm: LLMClient;
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  youtubeSearch?: YoutubeSearchFn;
  /** 1 ユーザ発話に対する AI 応答数 (既定 1)。 */
  responsesPerTurn?: number;
  onUtterance?: (u: FlowUtteranceRecord) => void | Promise<void>;
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
  rng?: Rng;
  gamesDir?: string;
  /**
   * 壁打ち相手に充てるプールペルソナの id または name (G)。
   * 解決できたものを相手として使う。未指定/未解決なら従来の生成ペルソナ。
   */
  opponentPersonaIds?: string[];
}

/** submitUser の返り値。 */
export type SparringTurnResult =
  | { kind: "responses"; user: FlowUtteranceRecord; utterances: FlowUtteranceRecord[] }
  | { kind: "round-summary"; round: number; summary: string; aufhebung: string[] }
  | { kind: "ended"; reason: "command" | "guard"; conclusion: string; concluded: boolean };

function defaultRng(): number {
  return Math.random();
}

/**
 * 壁打ちセッション。start() で初期化し、submitUser() でユーザ発話ごとに進める。
 * ユーザは不定期に submitUser を呼べる (固定ターン待ちをしない)。
 */
export class SparringSession {
  readonly sessionId = randomUUID();
  paperId = "";
  private paper!: DiscussionPaper;
  private personas: FlowPersona[] = [];
  private syntheticOpinions: string[] = [];
  private roundUtterances: Array<{ personaName: string; text: string }> = [];
  private readonly allUtterances: FlowUtteranceRecord[] = [];
  private readonly allAufhebung: string[] = [];
  private round = 1;
  private aiTurnCount = 0;
  private ended = false;
  private investigation?: Awaited<ReturnType<typeof investigateTheme>>;

  constructor(
    private readonly theme: string,
    private readonly tags: readonly FlowTag[],
    private readonly deps: SparringDeps
  ) {}

  get isEnded(): boolean {
    return this.ended;
  }

  /** 調査 + ペーパー + ペルソナを初期化する (LLM 議論はまだ起こさない)。 */
  async start(): Promise<void> {
    const cfg = getConfig();
    const isLocal = cfg.llm.backend === "local";
    const log = this.deps.log ?? ((m) => console.log(`[sparring] ${m}`));
    const warn = this.deps.warn ?? ((m) => console.warn(`[sparring/warn] ${m}`));

    const investigation = await investigateTheme({
      theme: this.theme,
      tags: this.tags,
      gamesDir: this.deps.gamesDir,
      youtubeSearch: this.deps.youtubeSearch,
      youtubeMaxComments: cfg.flow.youtubeMaxComments,
      warn,
    });
    this.investigation = investigation;

    const supplement = (await import("./tags.js")).paperSupplement(this.tags);
    this.paperId = persistPaper(
      { sessionId: this.sessionId, theme: this.theme, tags: [...this.tags], mechanics: investigation.mechanics, supplement },
      FLOW
    );
    this.paper = {
      paperId: this.paperId,
      sessionId: this.sessionId,
      theme: this.theme,
      tags: [...this.tags],
      mechanics: investigation.mechanics,
      supplement,
      rounds: [],
    };

    const defaultModel = cfg.llm.model ?? "claude-haiku-4-5-20251001";
    // G: 相手ペルソナ指定があればプールから解決して相手に充てる。未指定/未解決なら従来生成。
    const opponents: FlowPersona[] = [];
    for (const ref of this.deps.opponentPersonaIds ?? []) {
      const found = findPoolPersona(ref);
      if (found) opponents.push(toFlowPersona(found, { role: "debater", defaultModel, isLocal }));
      else warn(`壁打ち相手「${ref}」がプールに見つかりません (スキップ)`);
    }
    if (opponents.length > 0) {
      this.personas = opponents;
      log(`壁打ち相手 (指定): ${opponents.map((p) => p.name).join(", ")}`);
    } else {
      this.personas = generateFlowPersonas({
        count: cfg.flow.personaCount,
        defaultModel,
        isLocal,
        rng: this.deps.rng ?? defaultRng,
      });
    }
    this.syntheticOpinions = this.tags.includes("機密") ? synthesizeOpinions(investigation.mechanics) : [];
    log(`壁打ち開始: "${this.theme}" (ペルソナ ${this.personas.length} 人, 上限 ${cfg.flow.sparringMaxTurns} ターン)`);
  }

  /**
   * ユーザ発話を 1 件受け取り、コマンド or 通常応答を処理する。
   */
  async submitUser(text: string): Promise<SparringTurnResult> {
    if (this.ended) {
      return { kind: "ended", reason: "command", conclusion: "(終了済み)", concluded: false };
    }

    const command = detectSparringCommand(text);

    if (command === "end") {
      return this.finalize("command");
    }
    if (command === "summarize") {
      return this.closeRound();
    }

    // ── 通常発話: ユーザ発話を記録し AI が応答 ──────────────────────────────
    const userRecord = await this.recordUser(text);

    const cfg = getConfig();
    const responsesPerTurn = Math.max(1, this.deps.responsesPerTurn ?? 1);
    const utterances: FlowUtteranceRecord[] = [];

    for (let i = 0; i < responsesPerTurn; i++) {
      // 暴走ガード: 上限到達で安全に打ち切り (結論まとめへ)
      if (this.aiTurnCount >= cfg.flow.sparringMaxTurns) {
        const ended = await this.finalize("guard");
        return ended;
      }
      const u = await this.generateResponse();
      if (u) utterances.push(u);
    }

    return { kind: "responses", user: userRecord, utterances };
  }

  /** ユーザ発話を flow_utterance に記録する。 */
  private async recordUser(text: string): Promise<FlowUtteranceRecord> {
    const record: FlowUtteranceRecord = {
      id: randomUUID(),
      sessionId: this.sessionId,
      paperId: this.paperId,
      round: this.round,
      turn: this.roundUtterances.length,
      personaId: "user",
      personaName: "あなた",
      role: "user",
      stance: "neutral",
      text: text.trim(),
      isError: false,
    };
    persistUtterance(record);
    this.roundUtterances.push({ personaName: record.personaName, text: record.text });
    this.allUtterances.push(record);
    if (this.deps.onUtterance) await this.deps.onUtterance(record);
    return record;
  }

  /** AI 応答を 1 件生成して記録する (T2 の発話生成を流用)。空応答は null。 */
  private async generateResponse(): Promise<FlowUtteranceRecord | null> {
    const rng = this.deps.rng ?? defaultRng;
    const warn = this.deps.warn ?? ((m) => console.warn(`[sparring/warn] ${m}`));
    const persona = pickRandomPersona(this.personas, rng);
    const stance = decideStance(persona, rng);

    const userVoices: ContextVoice[] = (this.deps.listExternalVoices ?? (() => []))([this.theme], 5);
    if (this.investigation?.youtubeUsed && this.investigation.youtubeComments.length > 0) {
      userVoices.push(
        ...this.investigation.youtubeComments.slice(0, 3).map((c) => ({ content: c, source: "youtube" }))
      );
    }

    const personaPaper = buildPersonaPaper({
      paper: this.paper,
      persona,
      stance,
      currentRoundUtterances: this.roundUtterances,
      userVoices,
      syntheticOpinions: this.syntheticOpinions,
    });
    const prompt = paperToPrompt(personaPaper, stance, persona);

    const logged = withCostLog(this.deps.llm, {
      flow: FLOW,
      sessionId: this.sessionId,
      round: this.round,
      turn: this.roundUtterances.length,
      role: persona.role,
      persona: persona.name,
      location: persona.role === "facilitator" ? "facilitator" : "utterance",
    });

    const result = await logged.invoke({ prompt, model: persona.model });

    let textOut: string;
    let isError = false;
    if (!result.ok) {
      textOut = `[エラー: ${result.error}]`;
      isError = true;
      warn(`壁打ち応答 LLM エラー: ${result.error}`);
    } else {
      textOut = result.text.trim();
      if (!textOut) return null; // 空応答はターンに数えない
    }

    const record: FlowUtteranceRecord = {
      id: randomUUID(),
      sessionId: this.sessionId,
      paperId: this.paperId,
      round: this.round,
      turn: this.roundUtterances.length,
      personaId: persona.id,
      personaName: persona.name,
      role: persona.role,
      stance,
      text: textOut,
      isError,
    };
    persistUtterance(record);
    this.roundUtterances.push({ personaName: persona.name, text: textOut });
    this.allUtterances.push(record);
    this.aiTurnCount += 1;
    if (this.deps.onUtterance) await this.deps.onUtterance(record);
    return record;
  }

  /** 「まとめて」: 現ラウンドをサマライズして締め、次ラウンドへ (続行可)。 */
  private async closeRound(): Promise<SparringTurnResult> {
    const warn = this.deps.warn ?? ((m) => console.warn(`[sparring/warn] ${m}`));
    const summaryResult = await summarizeRound({
      theme: this.theme,
      sessionId: this.sessionId,
      paperId: this.paperId,
      round: this.round,
      utterances: this.roundUtterances,
      stockedAufhebung: [...this.allAufhebung],
      llm: this.deps.llm,
      warn,
      flow: FLOW,
    });
    this.allAufhebung.push(...summaryResult.newAufhebung);
    this.paper.rounds.push({ round: this.round, summary: summaryResult.summary, aufhebung: summaryResult.newAufhebung });

    const closedRound = this.round;
    this.round += 1;
    this.roundUtterances = [];
    return {
      kind: "round-summary",
      round: closedRound,
      summary: summaryResult.summary,
      aufhebung: summaryResult.newAufhebung,
    };
  }

  /** 「おわり」/ ガード到達: 結論まとめを出してフロー終了。 */
  private async finalize(reason: "command" | "guard"): Promise<SparringTurnResult> {
    const warn = this.deps.warn ?? ((m) => console.warn(`[sparring/warn] ${m}`));

    // 現ラウンドに未締めの発話があれば先にサマライズ (止揚を結論材料に)
    if (this.roundUtterances.length > 0) {
      try {
        const s = await summarizeRound({
          theme: this.theme,
          sessionId: this.sessionId,
          paperId: this.paperId,
          round: this.round,
          utterances: this.roundUtterances,
          stockedAufhebung: [...this.allAufhebung],
          llm: this.deps.llm,
          warn,
          flow: FLOW,
        });
        this.allAufhebung.push(...s.newAufhebung);
      } catch (err) {
        warn(`終了時サマリ失敗: ${(err as Error).message}`);
      }
    }

    const conclusion = await generateConclusion({
      theme: this.theme,
      sessionId: this.sessionId,
      paperId: this.paperId,
      allUtterances: this.allUtterances,
      allAufhebung: this.allAufhebung,
      // 壁打ちは投票しないため世論なし (空の評価)。結論は止揚 + 直近発話から生成。
      voteResults: [],
      llm: this.deps.llm,
      warn,
      flow: FLOW,
    });

    this.ended = true;
    return { kind: "ended", reason, conclusion: conclusion.summary, concluded: conclusion.concluded };
  }
}
