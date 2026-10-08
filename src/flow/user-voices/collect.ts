/**
 * ユーザーの声の収集 (spec/feature/flow/user-voices.md)。
 *
 * ゲームごとに:
 *   1. Steam でアプリを解決し、リリース済みならレビューを取得 → KG に取込 → ベクトル化する。
 *   2. Steam に無い (見つからない / 未リリース) なら、Voluptas の「遊んだ感想」を取得 → 取込 → ベクトル化する。
 *   3. どちらも取れなければ何もしない (既存の外部の声検索に任せる)。
 * 取得・取込・ベクトル化は注入境界 (テストで差し替え可)。失敗は warn して次のゲームへ進む (議論を止めない)。
 */

import type { createCore } from "../../core/index.js";
import type { ExternalUtterance } from "../../crawler/sources/types.js";
import type { SteamApp } from "./steam-app.js";
import { VoluptasUnavailable } from "./voluptas-impressions.js";

type Core = ReturnType<typeof createCore>;

/** 声を集める対象ゲーム。 */
export interface GameRef {
  /** 表示・Voluptas 検索に使う名前 (議題そのもののこともある)。 */
  title: string;
  /** Steam 検索に使う語 (先頭から順に試す)。 */
  searchTerms: string[];
  /** Steam の候補が同じゲームか判定する文字列 (議題全文 or 指定ゲーム名)。 */
  matchAgainst: string;
}

export type VoiceChannel = "di" | "steam" | "youtube" | "glab" | "none";

export interface GameVoiceReport {
  title: string;
  channel: VoiceChannel;
  /** 取込に使った gameSlug (ゲーム単位の声の引き出しに使う)。none なら null。 */
  gameSlug: string | null;
  appId?: number;
  appName?: string;
  /** Steam にあるが未リリースだった。 */
  unreleased?: boolean;
  collected: number;
  imported: number;
  embedded: number;
  voluptasDown?: boolean;
}

export interface CollectUserVoicesDeps {
  core: Core;
  workspaceId: string;
  existingCount?: (game: GameRef) => number;
  fetchYoutube?: (game: GameRef, gameSlug: string) => Promise<ExternalUtterance[]>;
  resolveSteam: (game: GameRef) => Promise<SteamApp | null>;
  fetchSteam: (app: SteamApp, gameSlug: string) => Promise<ExternalUtterance[]>;
  /** Voluptas 未設定なら undefined (Steam に無いゲームは声を集めない)。 */
  fetchGlab?: (game: GameRef, gameSlug: string) => Promise<ExternalUtterance[]>;
  importItems: (core: Core, items: ExternalUtterance[], workspaceId: string) => number;
  /** 埋め込み未設定なら undefined (ベクトル化しない)。@returns 書き込んだ件数。 */
  embed?: (core: Core, utteranceIds: string[]) => Promise<number>;
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
}

/** importer と同じ決定的 id (`ext:<source>:<nativeId>`)。 */
export function utteranceIdOf(item: Pick<ExternalUtterance, "source" | "nativeId">): string {
  return `ext:${item.source}:${item.nativeId}`;
}

/** Steam のゲーム単位 slug (appId で一意)。 */
export function steamGameSlug(appId: number): string {
  return `steam-${appId}`;
}

/** GLAB 感想のゲーム単位 slug (正規化したゲーム名)。 */
export function glabGameSlug(title: string): string {
  const norm = title.normalize("NFKC").toLowerCase().replace(/\s+/g, "-").slice(0, 48);
  return `glab-${norm || "game"}`;
}

async function importAndEmbed(
  items: ExternalUtterance[],
  deps: CollectUserVoicesDeps
): Promise<{ imported: number; embedded: number }> {
  if (items.length === 0) return { imported: 0, embedded: 0 };
  const imported = deps.importItems(deps.core, items, deps.workspaceId);
  let embedded = 0;
  if (deps.embed) {
    try {
      // 再取込 (dedup で skip) された声も、ベクトルが無ければ埋める。
      embedded = await deps.embed(deps.core, items.map(utteranceIdOf));
    } catch (e) {
      deps.warn?.(`ユーザーの声のベクトル化に失敗 (キーワード検索で継続): ${(e as Error).message}`);
    }
  }
  return { imported, embedded };
}

/** ゲーム 1 本の声を集める。 */
export async function collectGameVoices(game: GameRef, deps: CollectUserVoicesDeps): Promise<GameVoiceReport> {
  const base = { title: game.title, collected: 0, imported: 0, embedded: 0 };
  const existing = deps.existingCount?.(game) ?? 0;
  if (existing > 0) return { ...base, channel: "di", gameSlug: null, collected: existing };
  let app: SteamApp | null = null;
  try {
    app = await deps.resolveSteam(game);
  } catch (e) {
    deps.warn?.(`Steam 解決に失敗 (${game.title}): ${(e as Error).message}`);
  }

  if (app?.released) {
    const gameSlug = steamGameSlug(app.appId);
    try {
      const items = await deps.fetchSteam(app, gameSlug);
      const { imported, embedded } = await importAndEmbed(items, deps);
      deps.log?.(`Steam「${app.name}」(${app.appId}): ${items.length} 件取得 → 取込 ${imported} / ベクトル化 ${embedded}`);
      if (items.length > 0) return { ...base, channel: "steam", gameSlug, appId: app.appId, appName: app.name, collected: items.length, imported, embedded };
    } catch (e) {
      deps.warn?.(`Steam レビュー取得に失敗 (${app.name}): ${(e as Error).message}`);
    }
  }

  if (deps.fetchYoutube) {
    try {
      const gameSlug = `youtube-${glabGameSlug(game.title)}`;
      const items = await deps.fetchYoutube(game, gameSlug);
      if (items.length > 0) {
        const counts = await importAndEmbed(items, deps);
        return { ...base, ...counts, channel: "youtube", gameSlug, collected: items.length };
      }
    } catch (e) { deps.warn?.(`YouTube 取得に失敗 (${game.title}): ${(e as Error).message}`); }
  }
  const unreleased = app && !app.released ? { unreleased: true, appId: app.appId, appName: app.name } : {};
  if (!deps.fetchGlab) return { ...base, ...unreleased, channel: "none", gameSlug: null };
  const gameSlug = glabGameSlug(game.title);
  try {
    const items = await deps.fetchGlab(game, gameSlug);
    if (items.length === 0) return { ...base, ...unreleased, channel: "none", gameSlug: null };
    const { imported, embedded } = await importAndEmbed(items, deps);
    deps.log?.(`Voluptas「${game.title}」: 感想 ${items.length} 件取得 → 取込 ${imported} / ベクトル化 ${embedded}`);
    return { ...base, ...unreleased, channel: "glab", gameSlug, collected: items.length, imported, embedded };
  } catch (e) {
    deps.warn?.(`Voluptas の感想取得に失敗 (${game.title}): ${(e as Error).message}`);
    return { ...base, ...unreleased, channel: "none", gameSlug: null, voluptasDown: e instanceof VoluptasUnavailable };
  }
}

/** 複数ゲームの声を順に集める (1 本ずつ。Steam / Voluptas へ同時アクセスしない)。 */
export async function collectUserVoices(
  games: readonly GameRef[],
  deps: CollectUserVoicesDeps
): Promise<GameVoiceReport[]> {
  const reports: GameVoiceReport[] = [];
  for (const game of games) reports.push(await collectGameVoices(game, deps));
  return reports;
}

/** スレッド/画面に出す収集結果の一言。 */
export function describeVoiceReport(r: GameVoiceReport): string {
  if (r.voluptasDown) return `「${r.title}」: Voluptas（Vo）が停止中、または接続できません。外部の声なしでも議論できます。`;
  if (r.channel === "di") return `「${r.title}」: Di の収集済みデータ ${r.collected} 件を参考として使用`;
  if (r.channel === "youtube") return `「${r.title}」: YouTube コメント ${r.collected} 件を参考として使用`;
  if (r.channel === "steam") {
    return `「${r.appName ?? r.title}」: Steam レビュー ${r.collected} 件 (取込 ${r.imported} / ベクトル化 ${r.embedded})`;
  }
  const prefix = r.unreleased ? `「${r.appName ?? r.title}」は Steam で未リリース。` : `「${r.title}」は Steam で見つからず。`;
  if (r.channel === "glab") {
    return `${prefix}Voluptas の遊んだ感想 ${r.collected} 件 (取込 ${r.imported} / ベクトル化 ${r.embedded})`;
  }
  return `「${r.title}」: 今回の取得先から外部の声を取得できませんでした。0 件でも議論できます（通信失敗・未設定の場合も含みます）`;
}
