/**
 * 情報ゲートの呼び出し側グルー (web / discord 共通)。
 *
 * config 解決 + Core open/close + LLM の withCostLog ラップ + 対象フロー判定をまとめ、
 * runInformationGate を「議論/改善フロー開始の直前」に差し込めるようにする。
 * ゲート無効 / 対象外フロー / Core 無し のときは null を返す
 * (呼び出し側は従来のカウント閾値 autoCrawl にフォールバックする)。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { createCore } from "../core/index.js";
import type { ContextVoice } from "./discussion-paper.js";
import type { FlowTag } from "./tags.js";
import type { FlowKind } from "./dispatch.js";
import { getConfig } from "../config.js";
import { withCostLog } from "./cost-logger.js";
import { isAutoCrawlSource, type AutoCrawlSource } from "./learning-autocrawl.js";
import {
  isDensity,
  runInformationGate,
  type InformationDensity,
  type InformationGateConfig,
  type InformationGateResult,
} from "./information-gate.js";

type Core = ReturnType<typeof createCore>;

export interface FlowGateArgs {
  kind: FlowKind;
  theme: string;
  tags: readonly FlowTag[];
  /** 評価用 LLM (素の client。 ここで withCostLog する)。 */
  llm: LLMClient;
  openCore?: () => Core;
  workspaceId: string;
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  /** コストログ用キー (web=sessionId / discord=threadId)。 */
  sessionId: string;
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
}

/** config の minDensity (string) を厳格な InformationDensity に倒す。 */
function resolveMinDensity(raw: string): InformationDensity {
  return isDensity(raw) ? raw : "moderate";
}

/**
 * 情報ゲートが有効なら議論/改善の開始前に実行する。
 * @returns 実行した場合は結果、 ゲート対象外なら null (= 従来 autoCrawl にフォールバック)。
 */
export async function gateBeforeFlow(args: FlowGateArgs): Promise<InformationGateResult | null> {
  const { kind, theme, tags, openCore, workspaceId, listExternalVoices, sessionId } = args;
  const flowCfg = getConfig().flow;
  const gateCfg = flowCfg.informationGating;
  const crawl = flowCfg.autoCrawl;

  if (!gateCfg.enabled) return null;
  if (kind !== "discussion" && kind !== "improvement") return null;
  if (!openCore) return null;

  // クロール対象ソース: config.flow.autoCrawl.sources の有効分。youtube は API キーがあれば自動追加。
  // website/steam は URL/appId がテーマだけからは決まらないため自動経路では除外する。
  const youtubeApiKey = process.env.DISCUTERE_YOUTUBE_API_KEY;
  const autoUsable = (s: string): boolean => s === "niconico" || (s === "youtube" && !!youtubeApiKey);
  const crawlSources = Array.from(
    new Set([...crawl.sources, ...(youtubeApiKey ? ["youtube"] : [])])
  ).filter((s): s is AutoCrawlSource => isAutoCrawlSource(s) && autoUsable(s));
  if (crawlSources.length === 0) return null;

  const log = args.log ?? (() => {});
  const warn = args.warn ?? (() => {});

  const config: InformationGateConfig = {
    enabled: true,
    minDensity: resolveMinDensity(gateCfg.minDensity),
    maxLearnIterations: gateCfg.maxLearnIterations,
    maxGapsPerIteration: gateCfg.maxGapsPerIteration,
    model: gateCfg.model || undefined,
  };

  const core = openCore();
  try {
    const llm = withCostLog(args.llm, { flow: kind, sessionId, location: "information-gate" });
    return await runInformationGate({
      core,
      theme,
      tags,
      workspaceId,
      llm,
      listExternalVoices,
      crawlSources,
      maxItems: crawl.maxItems,
      youtubeApiKey,
      config,
      log,
      warn,
    });
  } finally {
    core.close?.();
  }
}
