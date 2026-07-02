/**
 * 投票フェーズ (discussion.md step 6, respec 03)。
 *
 * flow.voterCount 人の中立投票者が、ラウンドの発話から最も的を射た意見を 1 つ選ぶ。
 * 投票者にはペルソナを注入しない (中立)。得票数 = 世論 = そのラウンドの主要意見。
 * 既存のリアクションスコア機構 (👀 等) は使わない (OVERVIEW §11-b)。
 *
 * N 票が同一プロンプトの強相関 (実質 1 票) にならないよう (A4):
 *   - 投票者ごとに評価レンズ (論理/根拠/テーマ適合) を巡回割当する
 *   - 候補の提示順を投票者ごとに rng シャッフルする (位置バイアスの脱相関)
 *   - 「2」等の提示順番号回答も受理する (番号回答の棄権扱いを解消)
 *   - 同点は先着でなく rng の同点くじ (早い発話へのバイアス除去・提示順に非依存)
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { Rng } from "./personas.js";
import { DEFAULT_VOTE_LENSES, type VoteLens } from "./vote-lenses.js";
import { shuffle } from "./turn-scheduler.js";
import { withCostLog } from "./cost-logger.js";
import { getFlowDb } from "./db/connection.js";

export interface RoundUtterance {
  id: string;
  personaName: string;
  text: string;
}

export interface VoteResult {
  /**
   * 集計の意味種別 (A10): "votes"=票数 / "scores"=機械スコア (improvement の design_gap 射影)。
   * 表示層はこれで「n 票」と「スコア x.xx」を出し分ける。未指定は "votes" (後方互換)。
   */
  kind?: "votes" | "scores";
  /** utterance_id ごとの得票数 (kind="scores" のときは射影スコア) */
  tally: Record<string, number>;
  /** 最多得票の utterance_id (同点は rng くじ)。投票者が 0 人または全員棄権なら null */
  winner: string | null;
  /**
   * 最多得票シェア = maxVotes / 有効票数 (収束シグナル用, respec 04)。
   * 有効票 0 / kind="scores" では算出不能のため null。
   */
  winnerShare?: number | null;
}

function buildVotePrompt(theme: string, utterances: readonly RoundUtterance[], lens: VoteLens): string {
  const list = utterances
    .map((u, i) => `${i + 1}. [ID:${u.id}] ${u.text}`)
    .join("\n");
  return (
    `議論のテーマ: ${theme}\n\n` +
    `このラウンドの発言一覧:\n${list}\n\n` +
    `あなたは中立な審判として、${lens.instruction}。` +
    "テーマに対して最も的を射た意見を 1 つ選び、" +
    " その発言の ID (または先頭の番号) だけを返してください。\n" +
    "返答形式 (IDのみ): <選んだID>\n" +
    "例: abc-123\n" +
    "返答はIDまたは番号のみ (説明不要)。"
  );
}

/**
 * 投票回答を候補 ID に解決する。
 *   1. ID 完全一致 / 部分一致 (LLM が余分なテキストを含む場合)
 *   2. 提示順の番号回答 ("2" / "2." / "2 番" 等、先頭の整数のみの回答) → presented の該当候補
 */
export function parseVoteId(
  text: string,
  validIds: Set<string>,
  presented: readonly string[] = []
): string | null {
  const trimmed = text.trim();
  // 完全一致
  if (validIds.has(trimmed)) return trimmed;
  // 部分一致 (LLM が余分なテキストを含む場合)
  for (const id of validIds) {
    if (trimmed.includes(id)) return id;
  }
  // 番号回答: 整数 + 任意の句点/「番」のみの回答を、その投票者に提示した順の候補に解決する。
  const m = trimmed.match(/^(\d{1,3})\s*(?:[.。]|番)?\s*$/);
  if (m) {
    const idx = Number(m[1]) - 1;
    if (idx >= 0 && idx < presented.length) return presented[idx];
  }
  return null;
}

/**
 * 最多得票の決定。同点は先着でなく rng の同点くじ (A4)。
 * 同点候補は id 昇順に並べてから引くため、提示順・発話順に依存しない。
 */
export function pickVoteWinner(
  tally: Record<string, number>,
  rng: Rng
): { winner: string | null; maxVotes: number } {
  let maxVotes = 0;
  for (const v of Object.values(tally)) {
    if (v > maxVotes) maxVotes = v;
  }
  if (maxVotes <= 0) return { winner: null, maxVotes: 0 };
  const top = Object.keys(tally)
    .filter((id) => tally[id] === maxVotes)
    .sort();
  const winner = top.length === 1 ? top[0] : top[Math.floor(rng() * top.length)];
  return { winner, maxVotes };
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
  /** 評価レンズ (config flow.voteLenses)。既定は DEFAULT_VOTE_LENSES。 */
  lenses?: readonly VoteLens[];
  /** 候補シャッフル・同点くじ用 (テストで決定的に注入)。既定 Math.random。 */
  rng?: Rng;
  warn?: (msg: string) => void;
}): Promise<VoteResult> {
  const {
    theme,
    sessionId,
    round,
    voterCount,
    utterances,
    llm,
    lenses = DEFAULT_VOTE_LENSES,
    rng = Math.random,
    warn = console.warn,
  } = args;

  if (utterances.length === 0) {
    return { kind: "votes", tally: {}, winner: null, winnerShare: null };
  }

  const validIds = new Set(utterances.map((u) => u.id));
  const tally: Record<string, number> = {};
  let validVotes = 0;

  const db = getFlowDb();

  for (let voterIdx = 0; voterIdx < voterCount; voterIdx++) {
    // レンズは巡回割当 (voterCount=3 なら 1 人ずつ、6 なら 2 人ずつ)。
    const lens = lenses.length > 0 ? lenses[voterIdx % lenses.length] : DEFAULT_VOTE_LENSES[0];
    // 候補提示順は投票者ごとにシャッフル (位置バイアスが集計に乗るのを防ぐ)。
    const presented = shuffle(utterances, rng);

    const logged = withCostLog(llm, {
      flow: "discussion",
      sessionId,
      round,
      location: "vote",
    });

    const result = await logged.invoke({
      prompt: buildVotePrompt(theme, presented, lens),
    });

    let chosenId: string | null = null;

    if (!result.ok) {
      warn(`投票 round=${round} voter=${voterIdx} (${lens.key}) 失敗: ${result.error}`);
    } else {
      chosenId = parseVoteId(result.text, validIds, presented.map((u) => u.id));
      if (!chosenId) {
        warn(
          `投票 round=${round} voter=${voterIdx} (${lens.key}): 有効な ID を解析できませんでした (${result.text.slice(0, 60)})`
        );
      } else {
        tally[chosenId] = (tally[chosenId] ?? 0) + 1;
        validVotes += 1;
      }
    }

    // 投票 DB に記録
    db.prepare(
      `INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES (?, ?, ?, ?, ?)`
    ).run(sessionId, round, voterIdx, chosenId, Date.now());
  }

  const { winner, maxVotes } = pickVoteWinner(tally, rng);
  const winnerShare = validVotes > 0 && winner ? maxVotes / validVotes : null;

  return { kind: "votes", tally, winner, winnerShare };
}
