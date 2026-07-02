/**
 * ファシリテーター開幕ターン (discussion.md step 4, respec 10 の runFlow 分割 + respec 02)。
 *
 * ラウンド開幕の議題提示を担う。round ≥ 2 では前ラウンドの結果 (サマリ / 止揚 / 世論) を
 * プロンプトに注入し、「同じ議論の N 回試行」でなく前ラウンドの到達点から議論を進めさせる (A8)。
 * facilitator は開幕ターン (turn 0) 専任 — ターンループには参加しない (respec 02)。
 */

import { randomUUID } from "node:crypto";
import type { LLMClient } from "../persona-engine/llm/client.js";
import type { FlowPersona } from "./personas.js";
import type { FlowUtteranceRecord } from "./director.js";
import { withCostLog } from "./cost-logger.js";

/** 開幕プロンプトに注入する前ラウンドの結果 (round ≥ 2 のみ)。 */
export interface PreviousRoundContext {
  round: number;
  summary: string;
  aufhebung: string[];
  /** 前ラウンドの世論 (最多得票の発話)。無投票 / 全員棄権なら null。 */
  winner: { personaName: string; text: string } | null;
}

/** ファシリテーター開幕プロンプトを組み立てる (テスト用に export)。 */
export function buildFacilitatorPrompt(
  theme: string,
  round: number,
  previousRound?: PreviousRoundContext | null,
  guidingIssues?: readonly string[]
): string {
  // 承認済み論点 (09): ペーパーゲートで人間が確認した論点を毎ラウンド参考注入する。
  const issuesBlock =
    guidingIssues && guidingIssues.length > 0
      ? `事前に整理された論点 (参考):\n${guidingIssues.map((s, i) => `${i + 1}. ${s}`).join("\n")}\n`
      : "";
  const base =
    `あなたは議論の進行役です。\n` +
    `テーマ「${theme}」について、ラウンド ${round} の議論を始めてください。\n` +
    issuesBlock;

  // round 1 は従来通り (文脈なし出題)。
  if (!previousRound) {
    return base + `参加者が意見を出しやすいよう、1〜2 文で議題を提示してください。`;
  }

  const aufLine =
    previousRound.aufhebung.length > 0 ? `\n止揚: ${previousRound.aufhebung.join(" / ")}` : "";
  const winnerLine = previousRound.winner
    ? `\n世論 (最多得票): ${previousRound.winner.personaName}: ${previousRound.winner.text.slice(0, 120)}`
    : "";

  return (
    base +
    `\n# 前ラウンドの結果\n${previousRound.summary}${aufLine}${winnerLine}\n\n` +
    `# 指示\n` +
    `前ラウンドの到達点を踏まえ、まだ深まっていない論点・対立が残る論点へ\n` +
    `議論を進める出題を 1〜2 文で行ってください。前ラウンドの繰り返しは避ける。`
  );
}

export interface FacilitatorTurnArgs {
  theme: string;
  round: number;
  facilitator: FlowPersona;
  sessionId: string;
  paperId: string;
  /** フロー種別 (コストログ用)。 */
  flow: string;
  llm: LLMClient;
  /** 前ラウンドの結果 (round ≥ 2 のとき注入)。 */
  previousRound?: PreviousRoundContext | null;
  /** 承認済み論点 (09、ペーパーゲート由来)。毎ラウンド参考注入する。 */
  guidingIssues?: readonly string[];
  warn: (msg: string) => void;
}

/**
 * 開幕ターンを 1 回実行し、発話レコードを返す (永続・通知は呼び出し側 director が行う —
 * onUtterance / persistUtterance の呼び出し順序を変えないため)。
 * LLM エラーは isError=true のレコードとして返す。空応答は null。
 */
export async function runFacilitatorTurn(args: FacilitatorTurnArgs): Promise<FlowUtteranceRecord | null> {
  const { theme, round, facilitator, sessionId, paperId, flow, llm, previousRound, guidingIssues, warn } = args;

  const facilitatorLogged = withCostLog(llm, {
    flow,
    sessionId,
    round,
    turn: 0,
    role: facilitator.role,
    persona: facilitator.name,
    location: "facilitator",
  });

  const result = await facilitatorLogged.invoke({
    prompt: buildFacilitatorPrompt(theme, round, previousRound, guidingIssues),
    model: facilitator.model,
  });

  let text: string;
  let isError = false;

  if (!result.ok) {
    text = `[エラー: ${result.error}]`;
    isError = true;
    warn(`ラウンド ${round} ファシリテーター開幕ターン エラー: ${result.error}`);
  } else {
    text = result.text.trim();
  }

  if (!text) return null;

  return {
    id: randomUUID(),
    sessionId,
    paperId,
    round,
    turn: 0,
    personaId: facilitator.id,
    personaName: facilitator.name,
    role: facilitator.role,
    stance: "neutral",
    text,
    isError,
  };
}
