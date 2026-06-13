/**
 * 結論生成 (discussion.md step 9)。
 *
 * 全ラウンドの止揚 + 高評価発話 (世論) を元に LLM で結論テキストを生成する。
 * 既存の buildConvergePrompt / extractJsonObject (facilitator/prompts.ts) を流用する。
 * 結論フォーマットは src/visualize/conclusions.ts の CONVERGE_PREFIX 形式に倣う。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import {
  buildConvergePrompt,
  extractJsonObject,
  type RecentUtterance,
} from "../persona-engine/facilitator/prompts.js";
import { withCostLog } from "./cost-logger.js";
import { getFlowDb } from "./db/connection.js";
import type { FlowUtteranceRecord } from "./director.js";
import type { VoteResult } from "./vote.js";

/** src/visualize/conclusions.ts の CONVERGE_PREFIX に倣う */
const CONVERGE_PREFIX = "【収束】";

export interface ConclusionResult {
  summary: string;
  concluded: boolean;
}

export interface GenerateConclusionArgs {
  theme: string;
  sessionId: string;
  paperId: string;
  allUtterances: FlowUtteranceRecord[];
  allAufhebung: string[];
  voteResults: VoteResult[];
  llm: LLMClient;
  warn?: (msg: string) => void;
  /** フロー種別 (コストログ用)。既定 "discussion"。 */
  flow?: string;
}

/**
 * 結論を生成して flow_conclusion テーブルに保存する。
 * aufhebung が空で発話も少ない場合は「結論なし」とする。
 */
export async function generateConclusion(args: GenerateConclusionArgs): Promise<ConclusionResult> {
  const {
    theme,
    sessionId,
    paperId,
    allUtterances,
    allAufhebung,
    voteResults,
    llm,
    warn = console.warn,
    flow = "discussion",
  } = args;

  // 世論 (投票で選ばれた発話 = winner utterance)
  const winnerIds = new Set(
    voteResults.map((r) => r.winner).filter((id): id is string => id !== null)
  );
  const topUtterances: RecentUtterance[] = allUtterances
    .filter((u) => winnerIds.has(u.id))
    .map((u) => ({ speaker: u.personaName, content: u.text }));

  const recentAll: RecentUtterance[] = allUtterances
    .slice(-20)
    .map((u) => ({ speaker: u.personaName, content: u.text }));

  const convergePrompt = buildConvergePrompt({
    topic: theme,
    recent: recentAll,
    aufhebungen: allAufhebung,
    topOpinions: topUtterances.map((u) => `${u.speaker}: ${u.content}`),
  });

  const logged = withCostLog(llm, {
    flow,
    sessionId,
    location: "summary",
  });

  let summary = "(結論なし)";
  let concluded = false;

  if (allAufhebung.length > 0 || topUtterances.length > 0) {
    const result = await logged.invoke({
      prompt: `${convergePrompt.system}\n\n${convergePrompt.user}`,
    });

    if (result.ok) {
      try {
        const parsed = extractJsonObject(result.text) as { summary: string };
        if (parsed.summary) {
          summary = `${CONVERGE_PREFIX}${parsed.summary}`;
          concluded = true;
        }
      } catch {
        // JSON パース失敗
        if (result.text.trim()) {
          summary = `${CONVERGE_PREFIX}${result.text.trim()}`;
          concluded = true;
        }
      }
    } else {
      warn(`結論生成 失敗: ${result.error}`);
    }
  }

  // DB に保存
  const db = getFlowDb();
  db.prepare(
    `INSERT OR REPLACE INTO flow_conclusion
       (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?)`
  ).run(
    sessionId,
    paperId,
    summary,
    JSON.stringify(allAufhebung),
    JSON.stringify([...winnerIds]),
    concluded ? 1 : 0,
    Date.now()
  );

  return { summary, concluded };
}
