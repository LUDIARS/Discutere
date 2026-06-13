/**
 * 共通ディスパッチ (t7-entrypoints.md / OVERVIEW §9)。
 *
 * (theme, tags, flow, scene) を受け、議論タイプ (flow) に応じて対応するドライバを起動する
 * 単一入口。WebUI / Discord フォーラム投稿の両経路がここに集約する。
 *
 *   - 議論 (discussion) → runDiscussionFlow (中立投票)
 *   - 改善 (improvement) → runImprovementFlow (design_gap 機械スコア)
 *   - 学習 (learning)   → runLearningFlow (収集のみ・LLM 議論なし)
 *   - 壁打ち (sparring)  → SparringSession (ユーザターン起点・対話継続)
 *
 * 議論タイプは **必須**。未指定/不正な投稿は受理しない (parseFlowKind が null → エラー)。
 * 壁打ちは投稿時の議論タイプ選択で判断する (発話内容での自動判定はしない)。
 */

import type { createCore } from "../core/index.js";
import type { LLMClient } from "../persona-engine/llm/client.js";
import type { CascadeClients } from "../crawler/sentiment/cascade.js";
import type { FlowTag } from "./tags.js";
import type { ContextVoice } from "./discussion-paper.js";
import type { YoutubeSearchFn } from "./investigate.js";
import { runDiscussionFlow, runFlow, type FlowDirectorResult, type FlowUtteranceRecord } from "./director.js";
import { runImprovementFlow } from "./improvement.js";
import { runLearningFlow, type LearningFlowResult, type LearningOpinion } from "./learning.js";
import { SparringSession } from "./sparring.js";
import type { GameMechanicEntry } from "./games-md.js";

type Core = ReturnType<typeof createCore>;

export type FlowKind = "discussion" | "improvement" | "learning" | "sparring";

/** 議論タイプのラベル (WebUI select 値 / Discord タグ名) → FlowKind。 */
const FLOW_KIND_ALIASES: Record<FlowKind, string[]> = {
  discussion: ["discussion", "議論", "ディスカッション", "討論"],
  improvement: ["improvement", "改善", "改善提案", "提案"],
  learning: ["learning", "学習", "収集", "感想収集"],
  sparring: ["sparring", "壁打ち", "壁打"],
};

/**
 * ラベル文字列を FlowKind に解決する。完全一致を優先し、無ければ部分一致。
 * 解決できなければ null (= 議論タイプ未指定/不正)。
 */
export function parseFlowKind(label: string | undefined | null): FlowKind | null {
  if (!label) return null;
  const l = label.trim().toLowerCase();
  if (!l) return null;
  // 完全一致
  for (const [kind, aliases] of Object.entries(FLOW_KIND_ALIASES) as [FlowKind, string[]][]) {
    if (aliases.some((a) => a.toLowerCase() === l)) return kind;
  }
  // 部分一致 (フォーラムタグ名に装飾が付く場合)
  for (const [kind, aliases] of Object.entries(FLOW_KIND_ALIASES) as [FlowKind, string[]][]) {
    if (aliases.some((a) => l.includes(a.toLowerCase()))) return kind;
  }
  return null;
}

export interface DispatchDeps {
  llm: LLMClient;
  /** Discatier Core (learning が KG/メカニクス書込に使う。learning を使うなら必須)。 */
  core?: Core;
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  youtubeSearch?: YoutubeSearchFn;
  /** learning フロー入力 (収集済み匿名意見 / メカニクス)。 */
  opinions?: LearningOpinion[];
  mechanics?: GameMechanicEntry[];
  /** learning の感情カスケード LLM。 */
  sentimentClients?: CascadeClients;
  /** learning の slug 明示 (日本語タイトル用)。 */
  slug?: string;
  /** 壁打ちの 1 ユーザ発話あたり応答数。 */
  responsesPerTurn?: number;
  onUtterance?: (u: FlowUtteranceRecord) => void | Promise<void>;
  rng?: () => number;
  gamesDir?: string;
  workspaceId?: string;
  /** WebUI が起動前に id を返すための固定 sessionId (discussion/improvement)。 */
  sessionId?: string;
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
  /**
   * テスト用にドライバを差し替える (既定は本物)。
   * フロー選択 → 正しいドライバ呼び出しの検証に使う。
   * 壁打ち (sparring) は SparringSession を直接構築するため差し替え対象外。
   */
  drivers?: Partial<{
    discussion: typeof runDiscussionFlow;
    improvement: typeof runImprovementFlow;
    learning: typeof runLearningFlow;
  }>;
}

export interface DispatchInput {
  theme: string;
  tags: readonly FlowTag[];
  /** 議論タイプ (ラベル or FlowKind)。必須。 */
  flow: string;
  /** トランスポート由来の scene (web:<room> / discord:<guild>/<thread>)。記録用。 */
  scene?: string;
}

export type DispatchResult =
  | { kind: "discussion" | "improvement"; flow: FlowKind; result: FlowDirectorResult }
  | { kind: "learning"; flow: FlowKind; result: LearningFlowResult }
  | { kind: "sparring"; flow: FlowKind; session: SparringSession };

/** flow が未指定/不正のとき投げる (議論タイプは必須)。 */
export class FlowTypeRequiredError extends Error {
  constructor(label: string | undefined) {
    super(`議論タイプが未指定または不正です: "${label ?? ""}"。投稿は受理されません。`);
    this.name = "FlowTypeRequiredError";
  }
}

/**
 * フローを起動する。flow=壁打ち のときは起動済み SparringSession を返す (対話継続用)。
 * 他フローは完走させて結果を返す。
 */
export async function dispatchFlow(input: DispatchInput, deps: DispatchDeps): Promise<DispatchResult> {
  const kind = parseFlowKind(input.flow);
  if (!kind) throw new FlowTypeRequiredError(input.flow);

  const { theme, tags } = input;
  const flowDeps = {
    llm: deps.llm,
    listExternalVoices: deps.listExternalVoices,
    youtubeSearch: deps.youtubeSearch,
    onUtterance: deps.onUtterance,
    rng: deps.rng,
    gamesDir: deps.gamesDir,
    sessionId: deps.sessionId,
    log: deps.log,
    warn: deps.warn,
  };

  switch (kind) {
    case "discussion": {
      const run = deps.drivers?.discussion ?? runDiscussionFlow;
      return { kind, flow: kind, result: await run(theme, tags, flowDeps) };
    }
    case "improvement": {
      const run = deps.drivers?.improvement ?? runImprovementFlow;
      return { kind, flow: kind, result: await run(theme, tags, flowDeps) };
    }
    case "learning": {
      const run = deps.drivers?.learning ?? runLearningFlow;
      if (!deps.core) throw new Error("learning フローには deps.core が必要です");
      const result = await run(theme, theme, {
        core: deps.core,
        opinions: deps.opinions,
        mechanics: deps.mechanics,
        sentimentClients: deps.sentimentClients,
        slug: deps.slug,
        workspaceId: deps.workspaceId,
        gamesDir: deps.gamesDir,
        log: deps.log,
        warn: deps.warn,
      });
      return { kind, flow: kind, result };
    }
    case "sparring": {
      const session = new SparringSession(theme, tags, {
        llm: deps.llm,
        listExternalVoices: deps.listExternalVoices,
        youtubeSearch: deps.youtubeSearch,
        responsesPerTurn: deps.responsesPerTurn,
        onUtterance: deps.onUtterance,
        rng: deps.rng,
        gamesDir: deps.gamesDir,
        log: deps.log,
        warn: deps.warn,
      });
      await session.start();
      return { kind, flow: kind, session };
    }
  }
}

// runFlow は dispatch では直接使わないが、再エクスポートで入口を 1 箇所に集約する。
export { runFlow };
