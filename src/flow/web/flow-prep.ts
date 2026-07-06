/**
 * 議論フロー WebUI の「開始前 情報準備」ヘルパ群 (routes.ts から分離 / SRP)。
 *
 * - ペーパー編集ゲートの有効判定 (webPaperGateEnabled)
 * - リクエストボディ + config から自動クロール指定を組み立て (buildAutoCrawlSpec / buildLearningCrawl)
 * - 議論/改善の開始前に情報を整える (情報ゲート → 旧 autoCrawl フォールバック)
 *
 * いずれも HTTP ハンドラ本体 (routes.ts) から呼ばれる。FlowWebDeps はここを単一定義とし
 * routes.ts は re-export する。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { CascadeClients } from "../../crawler/sentiment/cascade.js";
import type { createCore } from "../../core/index.js";
import type { ContextVoice } from "../discussion-paper.js";
import type { FlowKind } from "../dispatch.js";
import type { FlowTag } from "../tags.js";
import {
  ensureLearningData,
  isAutoCrawlSource,
  resolveAutoCrawlSources,
  deriveSlug,
  type AutoCrawlSpec,
} from "../learning-autocrawl.js";
import type { LearningCrawlSpec } from "../learning.js";
import { gateBeforeFlow } from "../information-gate-runner.js";
import { getConfig } from "../../config.js";

type Core = ReturnType<typeof createCore>;

export interface FlowWebDeps {
  workspaceId: string;
  /** フロー実行用 LLM クライアント。 */
  llm: LLMClient;
  /** Discatier Core を開くファクトリ (learning に必要)。 */
  openCore?: () => Core;
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  sentimentClients?: CascadeClients;
  gamesDir?: string;
  youtubeApiKey?: string | null;
  getYoutubeApiKey?: () => Promise<string | null>;
}

export async function resolveYoutubeApiKey(d: FlowWebDeps): Promise<string | null> {
  return d.getYoutubeApiKey ? await d.getYoutubeApiKey() : d.youtubeApiKey ?? null;
}

/** Web 議論/改善をペーパー編集ゲート経由にするか (enabled か webCanonical のどちらか)。 */
export function webPaperGateEnabled(): boolean {
  const pr = getConfig().flow.paperReview;
  return pr.enabled || pr.webCanonical;
}

/** リクエストボディ + config 既定から自動クロール指定を組み立てる。 enabled=false / 不正ソースは null。 */
export function buildAutoCrawlSpec(body: {
  learningSource?: unknown;
  learningQuery?: unknown;
  learningAppId?: unknown;
  learningUrls?: unknown;
  learningSubreddit?: unknown;
}): AutoCrawlSpec | null {
  const cfg = getConfig().flow.autoCrawl;
  if (!cfg.enabled) return null;
  const requested = typeof body.learningSource === "string" ? body.learningSource.trim() : "";
  const source = requested || cfg.source;
  if (!isAutoCrawlSource(source)) return null;
  const query = typeof body.learningQuery === "string" ? body.learningQuery.trim() : undefined;
  const appId =
    typeof body.learningAppId === "number"
      ? body.learningAppId
      : typeof body.learningAppId === "string" && body.learningAppId.trim() !== ""
        ? Number(body.learningAppId)
        : undefined;
  const urls = Array.isArray(body.learningUrls)
    ? body.learningUrls.filter((u): u is string => typeof u === "string" && u.trim() !== "")
    : typeof body.learningUrls === "string" && body.learningUrls.trim() !== ""
      ? body.learningUrls.split(/[\s,]+/).filter(Boolean)
      : undefined;
  const subreddit =
    typeof body.learningSubreddit === "string" && body.learningSubreddit.trim() !== ""
      ? body.learningSubreddit.trim()
      : undefined;
  return { source, query, appId, urls, subreddit };
}

/**
 * 学習フローの自動収集指定を組み立てる (① 類似ゲームの自動収集)。
 *   - UI が learningSource を明示した場合は、その単一ソース (steam/website は appId/urls 付き) を使う。
 *   - 未指定なら config.flow.autoCrawl の自動経路ソース横断 (niconico + キーがあれば youtube)。
 * 収集可能なソースが無ければ undefined (= 収集しない)。
 */
export function buildLearningCrawl(body: {
  learningSource?: unknown;
  learningQuery?: unknown;
  learningAppId?: unknown;
  learningUrls?: unknown;
  learningSubreddit?: unknown;
  learningBalanced?: unknown;
  youtubeApiKey?: string | null;
}): LearningCrawlSpec | undefined {
  const cfg = getConfig().flow.autoCrawl;
  if (!cfg.enabled) return undefined;
  const youtubeApiKey = body.youtubeApiKey ?? undefined;
  const requested = typeof body.learningSource === "string" ? body.learningSource.trim() : "";
  const balanced = requested === "balanced" || body.learningBalanced === true;

  if (balanced) {
    const appId =
      typeof body.learningAppId === "number"
        ? body.learningAppId
        : typeof body.learningAppId === "string" && body.learningAppId.trim() !== ""
          ? Number(body.learningAppId)
          : undefined;
    const subreddit =
      typeof body.learningSubreddit === "string" && body.learningSubreddit.trim() !== ""
        ? body.learningSubreddit.trim()
        : undefined;
    const sources: LearningCrawlSpec["sources"] = ["steam", "youtube", "reddit", "niconico"];
    return {
      sources,
      query: typeof body.learningQuery === "string" ? body.learningQuery.trim() || undefined : undefined,
      maxItems: Math.max(10, Math.floor(cfg.maxItems / sources.length)),
      youtubeApiKey,
      specBySource: {
        steam: { appId },
        reddit: { subreddit },
      },
    };
  }

  // UI 明示ソース: buildAutoCrawlSpec の解決 (appId/urls 込み) をそのまま単一ソースに倒す。
  if (requested) {
    const spec = buildAutoCrawlSpec(body);
    if (!spec) return undefined;
    return {
      sources: [spec.source],
      query: spec.query,
      maxItems: cfg.maxItems,
      youtubeApiKey,
      specBySource: { [spec.source]: { appId: spec.appId, urls: spec.urls, subreddit: spec.subreddit } },
    };
  }

  // 既定: config のソースを自動経路に絞って横断収集。
  const sources = resolveAutoCrawlSources(cfg.sources, youtubeApiKey);
  if (sources.length === 0) return undefined;
  return {
    sources,
    query: typeof body.learningQuery === "string" ? body.learningQuery.trim() || undefined : undefined,
    maxItems: cfg.maxItems,
    youtubeApiKey,
  };
}

/**
 * 議論/改善の開始前に情報を整える。
 *   1. 情報ゲート (LLM が情報密度を評価し、不足観点を狙って学習 → 再評価) が有効ならそれを実行。
 *   2. ゲート対象外 (無効/フロー違い/Core 無し) なら、従来のカウント閾値 autoCrawl にフォールバック。
 * いずれも失敗は議論を止めない (graceful)。
 */
export async function prepareInformationBeforeFlow(
  kind: FlowKind,
  theme: string,
  tags: readonly FlowTag[],
  sessionId: string,
  spec: AutoCrawlSpec | null,
  d: FlowWebDeps
): Promise<void> {
  // 情報ゲート優先 (LLM 評価 + 不足観点クロール)。
  try {
    const youtubeApiKey = await resolveYoutubeApiKey(d);
    const gate = await gateBeforeFlow({
      kind,
      theme,
      tags,
      llm: d.llm,
      openCore: d.openCore,
      workspaceId: d.workspaceId,
      listExternalVoices: d.listExternalVoices,
      sessionId,
      log: (m) => console.log(`[flow-gate] ${m}`),
      warn: (m) => console.warn(`[flow-gate] ${m}`),
      youtubeApiKey,
    });
    if (gate) return; // ゲートが学習まで担った
  } catch (e) {
    console.warn(`[flow-gate] 情報ゲート失敗 (議論は続行): ${(e as Error).message}`);
  }
  // フォールバック: 従来のカウント閾値 autoCrawl。
  await legacyAutoCrawlBeforeFlow(kind, theme, spec, d);
}

/**
 * 旧来の自動クロール (テーマの外部の声が minVoices 件未満なら指定ソースでクロール → 取込)。
 * core 未設定 / 自動クロール無効ならスキップ。クロール失敗は議論を止めない (graceful)。
 */
export async function legacyAutoCrawlBeforeFlow(
  kind: FlowKind,
  theme: string,
  spec: AutoCrawlSpec | null,
  d: FlowWebDeps
): Promise<void> {
  if (!spec || !d.openCore || (kind !== "discussion" && kind !== "improvement")) return;
  const cfg = getConfig().flow.autoCrawl;
  const core = d.openCore();
  try {
    const youtubeApiKey = await resolveYoutubeApiKey(d);
    const result = await ensureLearningData({
      core,
      theme,
      slug: deriveSlug(theme),
      workspaceId: d.workspaceId,
      spec,
      minVoices: cfg.minVoices,
      maxItems: cfg.maxItems,
      listExternalVoices: d.listExternalVoices,
      youtubeApiKey: youtubeApiKey ?? undefined,
      log: (m) => console.log(`[flow-autocrawl] ${m}`),
      warn: (m) => console.warn(`[flow-autocrawl] ${m}`),
    });
    if (!result.skipped) console.log(`[flow-autocrawl] ${result.message}`);
  } catch (e) {
    console.warn(`[flow-autocrawl] クロール失敗 (議論は続行): ${(e as Error).message}`);
  } finally {
    core.close?.();
  }
}
