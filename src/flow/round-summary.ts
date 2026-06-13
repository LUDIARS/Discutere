/**
 * ラウンドサマリ + 止揚 (discussion.md step 7)。
 *
 * 既存の buildAufhebungJudgePrompt / extractJsonObject (facilitator/prompts.ts) を流用。
 * ラウンドの発話を LLM でサマライズし、止揚があれば抽出してペーパーに追記する。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import {
  buildAufhebungJudgePrompt,
  extractJsonObject,
  type RecentUtterance,
} from "../persona-engine/facilitator/prompts.js";
import { withCostLog } from "./cost-logger.js";
import { getFlowDb } from "./db/connection.js";

export interface RoundSummaryInput {
  theme: string;
  sessionId: string;
  paperId: string;
  round: number;
  utterances: Array<{ personaName: string; text: string }>;
  /** 過去ラウンドで蓄積済みの止揚 */
  stockedAufhebung: string[];
  llm: LLMClient;
  warn?: (msg: string) => void;
  /** フロー種別 (コストログ用)。既定 "discussion"。 */
  flow?: string;
}

export interface RoundSummaryOutput {
  summary: string;
  aufhebung: string[];
  /** 今ラウンドで新しく追加された止揚 */
  newAufhebung: string[];
}

function buildSummaryPrompt(theme: string, utterances: Array<{ personaName: string; text: string }>): string {
  const lines = utterances.map((u) => `- ${u.personaName}: ${u.text}`).join("\n");
  return (
    `議題: ${theme}\n\n以下はこのラウンドで出た意見です:\n${lines}\n\n` +
    "このラウンドの議論を 2〜3 文で要約してください。対立点と共通点を明確にする。\n" +
    "返答はサマリテキストのみ (JSON 不要)。"
  );
}

/**
 * ラウンドの発話をサマライズし、止揚を検出して DB に追記する。
 */
export async function summarizeRound(input: RoundSummaryInput): Promise<RoundSummaryOutput> {
  const {
    theme,
    sessionId,
    paperId,
    round,
    utterances,
    stockedAufhebung,
    llm,
    warn = console.warn,
    flow = "discussion",
  } = input;

  const recent: RecentUtterance[] = utterances.map((u) => ({
    speaker: u.personaName,
    content: u.text,
  }));

  // ── サマリ生成 ──────────────────────────────────────────────────────────
  let summary = `ラウンド ${round} 完了 (${utterances.length} 発話)`;

  const summaryLogged = withCostLog(llm, {
    flow,
    sessionId,
    round,
    location: "summary",
  });

  const summaryResult = await summaryLogged.invoke({
    prompt: buildSummaryPrompt(theme, utterances),
  });

  if (summaryResult.ok && summaryResult.text.trim()) {
    summary = summaryResult.text.trim();
  } else if (!summaryResult.ok) {
    warn(`サマリ生成 round=${round} 失敗: ${summaryResult.error}`);
  }

  // ── 止揚検出 ────────────────────────────────────────────────────────────
  const newAufhebung: string[] = [];

  if (utterances.length >= 2) {
    const aufPrompt = buildAufhebungJudgePrompt({
      topic: theme,
      recent,
      stockedSummaries: stockedAufhebung,
    });

    const aufLogged = withCostLog(llm, {
      flow,
      sessionId,
      round,
      location: "summary",
    });

    const aufResult = await aufLogged.invoke({
      prompt: `${aufPrompt.system}\n\n${aufPrompt.user}`,
    });

    if (aufResult.ok) {
      try {
        const parsed = extractJsonObject(aufResult.text) as { found: boolean; summary: string };
        if (parsed.found && parsed.summary) {
          newAufhebung.push(parsed.summary);
        }
      } catch {
        // JSON パース失敗は無視 (止揚なしとして扱う)
      }
    } else {
      warn(`止揚検出 round=${round} 失敗: ${aufResult.error}`);
    }
  }

  const aufhebung = [...stockedAufhebung, ...newAufhebung];

  // ── DB 追記 ─────────────────────────────────────────────────────────────
  const db = getFlowDb();
  // discussion_paper_round を更新 (T2 の placeholder を上書き)
  db.prepare(
    `UPDATE discussion_paper_round SET summary = ?, aufhebung_json = ? WHERE paper_id = ? AND round = ?`
  ).run(summary, JSON.stringify(newAufhebung), paperId, round);

  // 行が無い場合は挿入 (冪等)
  const existing = db
    .prepare("SELECT id FROM discussion_paper_round WHERE paper_id = ? AND round = ?")
    .get(paperId, round);
  if (!existing) {
    db.prepare(
      `INSERT INTO discussion_paper_round (paper_id, round, summary, aufhebung_json, created_at) VALUES (?, ?, ?, ?, ?)`
    ).run(paperId, round, summary, JSON.stringify(newAufhebung), Date.now());
  }

  return { summary, aufhebung, newAufhebung };
}
