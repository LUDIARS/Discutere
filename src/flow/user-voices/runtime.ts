/**
 * ユーザーの声の収集の入口 (spec/feature/flow/user-voices.md)。
 *
 * 議論/改善の準備時に呼ぶ: 議題のゲーム + 類似ゲームの声を集めて取り込み・ベクトル化し、
 * 議論に渡す「ゲームごとに均等に混ぜた」外部の声検索を返す。既定の実装 (Steam / Voluptas /
 * 取込 / 埋め込み) をここで束ね、収集ロジック本体 (collect.ts) は注入境界のまま保つ。
 */

import { getConfig } from "../../config.js";
import type { createCore } from "../../core/index.js";
import { createOpenAiCompatEmbedder } from "../../core/vectors/embedder.js";
import { embedVoicesByIds } from "../../core/vectors/voice-index.js";
import { openAttributionStore } from "../../crawler/sources/attribution-store.js";
import { fetchSteamReviews } from "../../crawler/sources/steam.js";
import { extractKeyTerms } from "../../discatier-engine-adapter/keyword-terms.js";
import {
  createServiceTokenProvider,
  serviceTokenClientConfigFromEnv,
} from "../../cernere-service-token/service-token-client.js";
import type { ContextVoice } from "../discussion-paper.js";
import { defaultImport } from "../learning-autocrawl.js";
import { classifyPaperReviewIntent } from "../paper-review-intent.js";
import { resolveVoluptasBearer, VOLUPTAS_TARGET_PROJECT_KEY } from "../voluptas-persona-client.js";
import { collectUserVoices, type CollectUserVoicesDeps, type GameRef, type GameVoiceReport } from "./collect.js";
import { listGameVoices } from "./game-voices.js";
import { makeMixedVoiceLookup } from "./mix.js";
import { resolveSteamApp } from "./steam-app.js";
import { fetchVoluptasImpressions } from "./voluptas-impressions.js";

type Core = ReturnType<typeof createCore>;
type VoiceLookup = (terms: string[], limit: number) => ContextVoice[];

/** 「」『』で囲まれた語 (ゲーム名の明示)。 */
function quotedTerms(text: string): string[] {
  return [...text.matchAll(/[「『]([^」』]{2,40})[」』]/g)].map((m) => m[1].trim());
}

/** 議題のゲーム (議題全文を match 対象に、明示名 → 議題の語の順で Steam 検索)。 */
export function themeGameRef(theme: string): GameRef {
  return {
    title: theme,
    searchTerms: [...quotedTerms(theme), ...extractKeyTerms([theme])],
    matchAgainst: theme,
  };
}

/** 類似ゲーム (指定名そのもので検索・判定)。 */
export function similarGameRef(name: string): GameRef {
  return { title: name, searchTerms: [name], matchAgainst: name };
}

/**
 * 最初の投稿などの本文から類似ゲームの指定を取り出す
 * (「類似ゲーム「A」「B」」など、ペーパー調整の類似ゲーム指定と同じ書き方)。
 */
export function parseSimilarGames(text: string | undefined): string[] {
  if (!text) return [];
  const intent = classifyPaperReviewIntent(text);
  return intent.kind === "similar_games" ? intent.terms : [];
}

/** 既定の収集依存 (Steam / Voluptas / 取込 / 埋め込み) を組む。 */
export function defaultCollectDeps(core: Core, workspaceId: string, log: (m: string) => void, warn: (m: string) => void): CollectUserVoicesDeps {
  const cfg = getConfig();
  const uv = cfg.flow.userVoices;
  const embedder = cfg.embedding.enabled ? createOpenAiCompatEmbedder(cfg.embedding) : null;
  const serviceToken = createServiceTokenProvider({
    targetProjectKey: VOLUPTAS_TARGET_PROJECT_KEY,
    config: () => serviceTokenClientConfigFromEnv(),
  });
  return {
    core,
    workspaceId,
    resolveSteam: (game) => resolveSteamApp({ terms: game.searchTerms, matchAgainst: game.matchAgainst }),
    fetchSteam: (app, gameSlug) =>
      fetchSteamReviews({ appId: app.appId, gameSlug, languages: uv.steamLanguages, maxReviews: uv.steamMaxReviews }),
    fetchGlab: uv.voluptasBaseUrl
      ? async (game, gameSlug) =>
          fetchVoluptasImpressions({
            baseUrl: uv.voluptasBaseUrl,
            bearer: await resolveVoluptasBearer({
              serviceToken,
              fallbackToken: process.env.DISCUTERE_VOLUPTAS_EXPORT_TOKEN ?? "",
              log: warn,
            }),
            game: game.title,
            gameSlug,
            limit: uv.glabMaxImpressions,
          })
      : undefined,
    importItems: defaultImport,
    embed: embedder
      ? (c, ids) => embedVoicesByIds(c, embedder, ids, { workspaceId, batchSize: cfg.embedding.batchSize })
      : undefined,
    log,
    warn,
  };
}

export interface PreparedUserVoices {
  reports: GameVoiceReport[];
  /**
   * 議論/ペーパーへ渡す外部の声検索。集めた声があればゲームごとに均等に混ぜる。
   * 議題のゲームの声が集まらなかった場合は、議題側だけ既存のキーワード検索 (base) を使う。
   */
  lookup: VoiceLookup | undefined;
}

/**
 * 議題のゲーム + 類似ゲームの声を集め、均等混合の検索を返す。
 * 無効化・Core 未設定・全滅のときは lookup=undefined (呼び出し側は既存の検索を使う)。
 */
export async function prepareUserVoices(args: {
  theme: string;
  similarGames: readonly string[];
  openCore?: () => Core;
  workspaceId: string;
  /** 既存の外部の声検索 (議題のゲームの声が集まらなかった時に使う)。 */
  baseLookup?: VoiceLookup;
  log?: (m: string) => void;
  warn?: (m: string) => void;
  /** テスト用: 収集依存の差し替え。 */
  collectDeps?: (core: Core) => CollectUserVoicesDeps;
}): Promise<PreparedUserVoices> {
  const cfg = getConfig();
  const log = args.log ?? (() => {});
  const warn = args.warn ?? (() => {});
  if (!cfg.flow.userVoices.enabled || !args.openCore) return { reports: [], lookup: undefined };

  const games = [themeGameRef(args.theme), ...args.similarGames.map(similarGameRef)];
  const core = args.openCore();
  let reports: GameVoiceReport[];
  let queryVector: number[] | null = null;
  try {
    const deps = args.collectDeps?.(core) ?? defaultCollectDeps(core, args.workspaceId, log, warn);
    reports = await collectUserVoices(games, deps);
    if (cfg.embedding.enabled && reports.some((r) => r.gameSlug)) {
      try {
        [queryVector] = await createOpenAiCompatEmbedder(cfg.embedding).embed([args.theme]);
      } catch (e) {
        warn(`議題の埋め込みに失敗 (新しい順で使う): ${(e as Error).message}`);
      }
    }
  } finally {
    core.close?.();
  }

  if (!reports.some((r) => r.gameSlug)) return { reports, lookup: undefined };

  const openCore = args.openCore;
  const lookup = makeMixedVoiceLookup((terms, perGroup) => {
    const c = openCore();
    const attribution = openAttributionStore();
    try {
      return reports.map((r, i) => {
        if (r.gameSlug) {
          return listGameVoices({ core: c, attribution, workspaceId: args.workspaceId, gameSlug: r.gameSlug, limit: perGroup, queryVector });
        }
        // 議題のゲームの声が集まらなければ、議題側は既存のキーワード検索で埋める。
        return i === 0 && args.baseLookup ? args.baseLookup(terms, perGroup) : [];
      });
    } catch (e) {
      warn(`ユーザーの声の引き出しに失敗 (既存の検索で継続): ${(e as Error).message}`);
      return [args.baseLookup ? args.baseLookup(terms, perGroup) : []];
    } finally {
      attribution.close();
      c.close?.();
    }
  });
  return { reports, lookup };
}
