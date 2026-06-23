/**
 * 議論開始時の自動学習クロール (事前学習の UI 化)。
 *
 * 旧設計では「議論の前に CLI で ext-ingest して学習データを仕込む」必要があり、UI からは
 * ほぼ不可能だった。ここは議論/改善フロー開始の直前に呼ばれ:
 *   1. テーマの学習データ (外部の声) が足りているか判定する (議論と同じ listExternalVoices)。
 *   2. 不足していれば「指定された方法 (ソース)」でクロール → KG 取込してから議論を始める。
 *
 * ソースとパラメータは議論ごとに UI で指定でき (web/routes.ts)、既定は config.flow.autoCrawl。
 * collector は DI 境界 (DEFAULT_COLLECTORS) にして、ネットワーク/API キー無しでテストできる。
 * クロール失敗は議論を止めない (呼び出し側が catch して続行する想定 / graceful degrade)。
 */

import type { createCore } from "../core/index.js";
import type { ContextVoice } from "./discussion-paper.js";
import type { ExternalUtterance } from "../crawler/sources/types.js";
import { toSlug } from "../ludus/economy-analyzer.js";
import { importExternalUtterances } from "../crawler/sources/importer.js";
import { openIngestedStore } from "../crawler/sources/ingested-store.js";
import { openAttributionStore } from "../crawler/sources/attribution-store.js";
import { fetchNiconicoDiscussions } from "../crawler/sources/niconico.js";
import { fetchSteamReviews } from "../crawler/sources/steam.js";
import { fetchWebsiteArticles } from "../crawler/sources/website.js";
import { discoverVideosBySearch } from "../crawler/sources/youtube-videos.js";
import { fetchVideoComments } from "../crawler/sources/youtube-comments.js";

type Core = ReturnType<typeof createCore>;

/** 自動クロールがサポートするソース (= UI の選択肢)。 */
export type AutoCrawlSource = "niconico" | "youtube" | "steam" | "website";

export const AUTO_CRAWL_SOURCES: AutoCrawlSource[] = ["niconico", "youtube", "steam", "website"];

export function isAutoCrawlSource(s: string): s is AutoCrawlSource {
  return (AUTO_CRAWL_SOURCES as string[]).includes(s);
}

/**
 * テーマだけから自動でクロールできるソースに絞る (情報ゲート / 学習フローの自動経路共通)。
 *   - niconico は常に可 (キー不要)。
 *   - youtube は API キーがあれば可。キーがあれば候補に自動追加する。
 *   - website / steam は URL / appId がテーマだけからは決まらないため自動経路では除外する
 *     (UI / Discord の明示指定時のみ使う)。
 */
export function resolveAutoCrawlSources(
  sources: readonly string[],
  youtubeApiKey?: string
): AutoCrawlSource[] {
  const autoUsable = (s: string): boolean => s === "niconico" || (s === "youtube" && !!youtubeApiKey);
  return Array.from(new Set([...sources, ...(youtubeApiKey ? ["youtube"] : [])])).filter(
    (s): s is AutoCrawlSource => isAutoCrawlSource(s) && autoUsable(s)
  );
}

/** テーマ → クロール発話に付ける gameSlug。 日本語は toSlug が空になるためフォールバックする。 */
export function deriveSlug(theme: string): string {
  return toSlug(theme) || theme.trim().replace(/\s+/g, "-").slice(0, 48) || "discussion";
}

/** クロール方法の指定 (UI 上書き or config 既定)。 */
export interface AutoCrawlSpec {
  source: AutoCrawlSource;
  /** 検索クエリ (niconico/youtube)。省略時はテーマ文字列。 */
  query?: string;
  /** Steam appId (steam では必須)。 */
  appId?: number;
  /** 取得対象 URL 群 (website では必須)。 */
  urls?: string[];
}

export interface CollectorContext {
  gameSlug: string;
  theme: string;
  spec: AutoCrawlSpec;
  maxItems: number;
  youtubeApiKey?: string;
}

export type Collector = (ctx: CollectorContext) => Promise<ExternalUtterance[]>;

function resolveQuery(ctx: CollectorContext): string {
  const q = ctx.spec.query?.trim();
  return q && q.length > 0 ? q : ctx.theme;
}

/** 既定 collector 群 (本物のネットワーク取得)。テストでは差し替える。 */
export const DEFAULT_COLLECTORS: Record<AutoCrawlSource, Collector> = {
  niconico: (ctx) =>
    fetchNiconicoDiscussions({
      gameSlug: ctx.gameSlug,
      query: resolveQuery(ctx),
      maxVideos: 5,
      maxComments: ctx.maxItems,
    }),
  steam: (ctx) => {
    if (!ctx.spec.appId) throw new Error("steam クロールには appId が必要です");
    return fetchSteamReviews({
      appId: ctx.spec.appId,
      gameSlug: ctx.gameSlug,
      maxReviews: ctx.maxItems,
    });
  },
  website: (ctx) => {
    if (!ctx.spec.urls || ctx.spec.urls.length === 0) {
      throw new Error("website クロールには 1 つ以上の URL が必要です");
    }
    return fetchWebsiteArticles({ urls: ctx.spec.urls, gameSlug: ctx.gameSlug });
  },
  youtube: async (ctx) => {
    if (!ctx.youtubeApiKey) {
      throw new Error("youtube クロールには DISCUTERE_YOUTUBE_API_KEY が必要です");
    }
    const videos = await discoverVideosBySearch({
      query: resolveQuery(ctx),
      apiKey: ctx.youtubeApiKey,
      maxResults: 5,
      order: "relevance",
    });
    const out: ExternalUtterance[] = [];
    for (const v of videos) {
      if (out.length >= ctx.maxItems) break;
      try {
        const comments = await fetchVideoComments({
          videoId: v.videoId,
          gameSlug: ctx.gameSlug,
          apiKey: ctx.youtubeApiKey,
          maxComments: ctx.maxItems - out.length,
        });
        out.push(...comments);
      } catch {
        // 1 動画の失敗は全体を止めない
      }
    }
    return out.slice(0, ctx.maxItems);
  },
};

export interface EnsureLearningDataDeps {
  core: Core;
  theme: string;
  /** クロール時に発話へ付ける gameSlug (検索検出はキーワードなので緩くてよい)。 */
  slug: string;
  workspaceId: string;
  spec: AutoCrawlSpec;
  /** これ未満の外部の声しか無ければクロールする。 */
  minVoices: number;
  /** 1 回の取込上限。 */
  maxItems?: number;
  /** 議論と同じ外部の声検索 (= 充足判定に流用)。 未指定なら常に不足扱い。 */
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  youtubeApiKey?: string;
  /** collector 差し替え (テスト用)。 */
  collectors?: Partial<Record<AutoCrawlSource, Collector>>;
  /** 取込関数 差し替え (テスト用)。 既定は ingested/attribution sidecar 付き import。 */
  importItems?: (core: Core, items: ExternalUtterance[], workspaceId: string) => number;
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
}

export interface EnsureLearningDataResult {
  /** 実際にクロール → 取込したか。 */
  crawled: boolean;
  /** クロール前の充足判定に使った外部の声件数。 */
  voicesBefore: number;
  /** 取込件数。 */
  imported: number;
  /** 既に充分で skip したか。 */
  skipped: boolean;
  message: string;
}

/** テーマに対する既存の外部の声件数 (議論と同じ検索)。 */
export function countLearningVoices(
  theme: string,
  listExternalVoices: EnsureLearningDataDeps["listExternalVoices"],
  limit: number
): number {
  if (!listExternalVoices) return 0;
  try {
    return listExternalVoices([theme], limit).length;
  } catch {
    return 0;
  }
}

function defaultImport(core: Core, items: ExternalUtterance[], workspaceId: string): number {
  const ingested = openIngestedStore();
  const attribution = openAttributionStore();
  try {
    return importExternalUtterances(core, items, { workspaceId, ingested, attribution }).utterances;
  } finally {
    attribution.close();
    ingested.close();
  }
}

export interface CollectAndImportDeps {
  core: Core;
  theme: string;
  /** クロール発話に付ける gameSlug。 */
  slug: string;
  workspaceId: string;
  spec: AutoCrawlSpec;
  /** 1 回の取込上限。既定 200。 */
  maxItems?: number;
  youtubeApiKey?: string;
  /** collector 差し替え (テスト用)。 */
  collectors?: Partial<Record<AutoCrawlSource, Collector>>;
  /** 取込関数 差し替え (テスト用)。 */
  importItems?: (core: Core, items: ExternalUtterance[], workspaceId: string) => number;
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
}

export interface CollectAndImportResult {
  /** collector が取得した件数。 */
  collected: number;
  /** KG に取り込んだ件数。 */
  imported: number;
}

/**
 * 充足判定を挟まず、指定ソース/クエリで「取得 → 取込」だけを行う (情報ゲートの狙い撃ち学習で流用)。
 * 充足ゲート付きの入口は ensureLearningData。
 */
export async function collectAndImport(deps: CollectAndImportDeps): Promise<CollectAndImportResult> {
  const {
    core,
    theme,
    slug,
    workspaceId,
    spec,
    maxItems = 200,
    youtubeApiKey,
    log = () => {},
    warn = () => {},
  } = deps;
  const importItems = deps.importItems ?? defaultImport;
  const collector = (deps.collectors ?? {})[spec.source] ?? DEFAULT_COLLECTORS[spec.source];
  if (!collector) {
    warn(`未対応のクロールソース: ${spec.source}`);
    return { collected: 0, imported: 0 };
  }
  const items = await collector({ gameSlug: slug, theme, spec, maxItems, youtubeApiKey });
  if (items.length === 0) return { collected: 0, imported: 0 };
  const imported = importItems(core, items, workspaceId);
  log(`${spec.source}: ${items.length} 件取得 → ${imported} 件を KG に取込`);
  return { collected: items.length, imported };
}

/**
 * テーマの学習データが不足していれば指定ソースでクロール → 取込する。
 * 充分なら no-op (skipped=true)。
 */
export async function ensureLearningData(
  deps: EnsureLearningDataDeps
): Promise<EnsureLearningDataResult> {
  const {
    core,
    theme,
    slug,
    workspaceId,
    spec,
    minVoices,
    maxItems = 200,
    listExternalVoices,
    youtubeApiKey,
    log = () => {},
    warn = () => {},
  } = deps;
  const importItems = deps.importItems ?? defaultImport;

  const voicesBefore = countLearningVoices(theme, listExternalVoices, Math.max(minVoices, 1));
  if (voicesBefore >= minVoices) {
    return {
      crawled: false,
      voicesBefore,
      imported: 0,
      skipped: true,
      message: `学習データ充分 (外部の声 ${voicesBefore} 件 ≥ ${minVoices})`,
    };
  }

  const collector = (deps.collectors ?? {})[spec.source] ?? DEFAULT_COLLECTORS[spec.source];
  if (!collector) {
    warn(`未対応のクロールソース: ${spec.source}`);
    return { crawled: false, voicesBefore, imported: 0, skipped: false, message: `未対応のソース: ${spec.source}` };
  }

  log(`学習データ不足 (外部の声 ${voicesBefore} 件 < ${minVoices}) → ${spec.source} でクロール開始`);
  const { collected, imported } = await collectAndImport({
    core,
    theme,
    slug,
    workspaceId,
    spec,
    maxItems,
    youtubeApiKey,
    collectors: deps.collectors,
    importItems,
    log,
    warn,
  });
  if (collected === 0) {
    return {
      crawled: false,
      voicesBefore,
      imported: 0,
      skipped: false,
      message: `${spec.source}: 取得 0 件 (議論はそのまま開始)`,
    };
  }
  return {
    crawled: imported > 0,
    voicesBefore,
    imported,
    skipped: false,
    message: `${spec.source} で ${collected} 件取得 → ${imported} 件を学習データに追加`,
  };
}
