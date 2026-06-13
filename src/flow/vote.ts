/**
 * 投票フェーズ (discussion.md step 6)。
 *
 * flow.voterCount 人の中立投票者が、ラウンドの発話から最も的を射た意見を 1 つ選ぶ。
 * 投票者にはペルソナを注入しない (中立)。得票数 = 世論 = そのラウンドの主要意見。
 * 既存のリアクションスコア機構 (👀 等) は使わない (OVERVIEW §11-b)。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import { withCostLog } from "./cost-logger.js";
import { getFlowDb } from "./db/connection.js";

export interface RoundUtterance {
  id: string;
  personaName: string;
  text: string;
}

export interface VoteResult {
  /** utterance_id ごとの得票数 */
  tally: Record<string, number>;
  /** 最多得票の utterance_id (同点は先着)。投票者が 0 人または全員棄権なら null */
  winner: string | null;
}

function buildVotePrompt(theme: string, utterances: RoundUtterance[]): string {
  const list = utterances
    .map((u, i) => `${i + 1}. [ID:${u.id}] ${u.personaName}: ${u.text}`)
    .join("\n");
  return (
    `議論のテーマ: ${theme}\n\n` +
    `このラウンドの発言一覧:\n${list}\n\n` +
    "あなたは中立な審判として、テーマに対して最も的を射た意見を 1 つ選び、" +
    " その発言の ID だけを返してください。\n" +
    "返答形式 (IDのみ): <選んだID>\n" +
    "例: abc-123\n" +
    "返答はIDのみ (説明不要)。"
  );
}

function parseVoteId(text: string, validIds: Set<string>): string | null {
  const trimmed = text.trim();
  // 完全一致
  if (validIds.has(trimmed)) return trimmed;
  // 部分一致 (LLM が余分なテキストを含む場合)
  for (const id of validIds) {
    if (trimmed.includes(id)) return id;
  }
  return null;
}

/**
 * ラウンド投票を実行し、vote テーブルに記録して結果を返す。
 */
export async function runRoundVote(args: {
  theme: string;
  sessionId: string;
  round: number;
  voterCount: number;
  utterances: RoundUtterance[];
  llm: LLMClient;
  warn?: (msg: string) => void;
}): Promise<VoteResult> {
  const { theme, sessionId, round, voterCount, utterances, llm, warn = console.warn } = args;

  if (utterances.length === 0) {
    return { tally: {}, winner: null };
  }

  const validIds = new Set(utterances.map((u) => u.id));
  const tally: Record<string, number> = {};

  const db = getFlowDb();

  for (let voterIdx = 0; voterIdx < voterCount; voterIdx++) {
    const logged = withCostLog(llm, {
      flow: "discussion",
      sessionId,
      round,
      location: "vote",
    });

    const result = await logged.invoke({
      prompt: buildVotePrompt(theme, utterances),
    });

    let chosenId: string | null = null;

    if (!result.ok) {
      warn(`投票 round=${round} voter=${voterIdx} 失敗: ${result.error}`);
    } else {
      chosenId = parseVoteId(result.text, validIds);
      if (!chosenId) {
        warn(`投票 round=${round} voter=${voterIdx}: 有効な ID を解析できませんでした (${result.text.slice(0, 60)})`);
      } else {
        tally[chosenId] = (tally[chosenId] ?? 0) + 1;
      }
    }

    // 投票 DB に記録
    db.prepare(
      `INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES (?, ?, ?, ?, ?)`
    ).run(sessionId, round, voterIdx, chosenId, Date.now());
  }

  // 最多得票を決定 (同点は先着 = utterances の順)
  let winner: string | null = null;
  let maxVotes = 0;
  for (const u of utterances) {
    const v = tally[u.id] ?? 0;
    if (v > maxVotes) {
      maxVotes = v;
      winner = u.id;
    }
  }

  return { tally, winner };
}
