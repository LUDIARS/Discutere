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
import { upsertDiscussionTitleCache, type RoundSummary } from "./discussion-paper.js";
import type { FlowUtteranceRecord } from "./director.js";
import type { VoteResult } from "./vote.js";
import { writeThroughFlowConclusion } from "../visualize/conclusion-cache.js";

/** src/visualize/conclusions.ts の CONVERGE_PREFIX に倣う */
const CONVERGE_PREFIX = "【収束】";

/**
 * 構造化出力 (JSON) に 2 回失敗して生テキストへ degrade した結論のプレフィックス
 * (respec 04, A7)。concluded=false のまま保存し、表示層で「未検証の収束」と区別できる。
 */
export const UNVERIFIED_PREFIX = "【収束(未検証)】";

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
  /**
   * 全ラウンドのサマリ (respec 04, A5)。converge プロンプトに「# 各ラウンドの要約」節として
   * 渡し、序盤ラウンドの内容が止揚以外すべて落ちる問題を解消する。省略時は節なし (後方互換)。
   */
  rounds?: RoundSummary[];
  voteResults: VoteResult[];
  llm: LLMClient;
  warn?: (msg: string) => void;
  /** フロー種別 (コストログ用)。既定 "discussion"。 */
  flow?: string;
  /**
   * 結論に併記する注記 (09: 議論適性: 低のまま強行した場合など)。
   * 生成された結論 (無ければ「結論なし」) の末尾に `※ 注記` として付き、そのまま永続される。
   */
  annotation?: string;
}

/** 全ラウンドサマリを converge プロンプトの節テキストにする (空なら "")。 */
export function buildRoundsSection(rounds: readonly RoundSummary[]): string {
  if (rounds.length === 0) return "";
  const lines = rounds.map((r) => {
    const auf = r.aufhebung.length > 0 ? `\n  止揚: ${r.aufhebung.join(" / ")}` : "";
    return `ラウンド ${r.round}: ${r.summary}${auf}`;
  });
  return `# 各ラウンドの要約\n${lines.join("\n")}\n\n`;
}

/** LLM 応答から結論 summary を取り出す (JSON パース + 非空検証)。失敗は null。 */
function tryParseSummary(text: string): string | null {
  try {
    const parsed = extractJsonObject(text) as { summary?: unknown };
    return typeof parsed.summary === "string" && parsed.summary.trim() ? parsed.summary : null;
  } catch {
    return null;
  }
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
    rounds = [],
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
    // 全ラウンドサマリを「# 各ラウンドの要約」節として結論入力に含める (A5)。
    const prompt = `${convergePrompt.system}\n\n${buildRoundsSection(rounds)}${convergePrompt.user}`;
    const result = await logged.invoke({ prompt });

    if (result.ok) {
      // concluded 判定の統一 (A7): 構造化出力の失敗は 1 回だけリトライし、
      // それでも失敗なら生テキストを【収束(未検証)】として保存するが concluded=false。
      let parsed = tryParseSummary(result.text);
      let rawText = result.text.trim();
      if (!parsed) {
        warn(`結論生成 JSON パース失敗 — 同一プロンプトで 1 回リトライ`);
        const retry = await logged.invoke({ prompt });
        if (retry.ok) {
          parsed = tryParseSummary(retry.text);
          if (retry.text.trim()) rawText = retry.text.trim();
        } else {
          warn(`結論生成 リトライ失敗: ${retry.error}`);
        }
      }
      if (parsed) {
        summary = `${CONVERGE_PREFIX}${parsed}`;
        concluded = true;
      } else if (rawText) {
        summary = `${UNVERIFIED_PREFIX}${rawText}`;
        concluded = false;
        warn(`結論生成: 構造化出力に 2 回失敗 — 生テキストを ${UNVERIFIED_PREFIX} として保存 (concluded=false)`);
      }
    } else {
      warn(`結論生成 失敗: ${result.error}`);
    }
  }

  // 注記 (議論適性: 低 の強行など) を結論に併記する (表示・永続の両方に乗る)。
  if (args.annotation && args.annotation.trim()) {
    summary = `${summary}\n\n※ ${args.annotation.trim()}`;
  }

  // DB に保存
  const db = getFlowDb();
  const now = Date.now();
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
    now
  );
  upsertDiscussionTitleCache(sessionId, concluded ? summary : theme, concluded ? "conclusion" : "paper", now, {
    discussionType: flow,
  });

  // 結論一覧キャッシュへ write-through (議論が収束するたびに発話数=議論ボリュームを更新)。
  // KG を引かない軽量更新。materialCount は別途 build:conclusion-cache で算出する。
  try {
    writeThroughFlowConclusion(db, {
      sessionId,
      title: theme,
      conclusion: concluded ? summary.slice(CONVERGE_PREFIX.length).trim() : null,
      utteranceCount: allUtterances.length,
      aufhebungCount: allAufhebung.length,
      updatedAt: now,
    });
  } catch (e) {
    warn(`結論キャッシュ更新 失敗 (議論は継続): ${(e as Error).message}`);
  }

  return { summary, concluded };
}
