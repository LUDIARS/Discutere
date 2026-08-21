/**
 * Step 4 — 集中収集: 横断検出した投稿者の全レビューを取得し、ペルソナ元データとして
 * ExternalUtterance 互換 JSONL に書き出す。spec/feature/crawler/STEAM-PERSONA.md §2-4。
 *
 * 出力はペルソナ形成側 (Voluptas / Histrio) が読む中立な JSONL。Di の KG には
 * ここでは入れない (追跡アプリは Game ノードを持たないため。取り込みたい場合は
 * 既存の ext-import に JSONL をそのまま渡せる形にしてある)。
 */

import fs from "node:fs";
import path from "node:path";

import type { ExternalUtterance } from "../sources/types.js";
import { isSteamId64, type SteamFetchPorts, type UserReviewEntry } from "./ports.js";
import type { SteamPersonaStore } from "./store.js";

export const DEFAULT_AUTHOR_DIR = path.resolve("./data/external/steam-persona/authors");

export interface CollectOptions {
  /** 1 実行で集中収集する投稿者数の上限 (polite なペース維持)。 */
  maxAuthorsPerRun: number;
  /** JSONL 出力先ディレクトリ。 */
  outDir: string;
  now: () => number;
  log?: (msg: string) => void;
}

export interface CollectResult {
  /** 収集を試みて収集済みに記録した投稿者数 (取得失敗の 0 件記録を含む)。 */
  authors: number;
  reviews: number;
  /** うち取得に失敗して 0 件で記録した投稿者数 (非公開プロフィール等)。 */
  failedAuthors: number;
}

/** @implements SPEC-STEAM-PERSONA-OUTPUT — ユーザ別レビューを ExternalUtterance に正規化する。 */
export function mapUserReviewToUtterance(entry: UserReviewEntry, fetchedAtMs: number): ExternalUtterance {
  return {
    source: "steam",
    // appreviews 由来 (recommendationid) と衝突しない user-review 系の nativeId。
    nativeId: `ur:${entry.steamId}:${entry.appId}`,
    // 追跡外アプリも含むため slug はアプリ id から機械導出する。
    gameSlug: `steam-app-${entry.appId}`,
    threadKey: String(entry.appId),
    content: entry.text,
    postedAt: fetchedAtMs, // プロフィールページの投稿日はロケール文字列のみ → 取得時刻で代替
    authorId: entry.steamId, // SteamID64 = 公開・安定な persona アンカー (spec §6)
    signal: entry.recommended === undefined ? undefined : { votedUp: entry.recommended },
    sourceUrl: entry.url,
  };
}

/** @implements SPEC-STEAM-PERSONA-OUTPUT — SteamID64 ごとの JSONL を安全な出力先へ書き出す。 */
function writeAuthorJsonl(outDir: string, steamId: string, items: ExternalUtterance[]): string {
  if (!isSteamId64(steamId)) throw new Error("steamId must be a SteamID64 before writing persona output");
  fs.mkdirSync(outDir, { recursive: true });
  const file = path.join(outDir, `${steamId}.jsonl`);
  fs.writeFileSync(file, items.map((i) => JSON.stringify(i)).join("\n") + (items.length ? "\n" : ""), "utf8");
  return file;
}

/** @implements SPEC-STEAM-PERSONA-PIPELINE, SPEC-STEAM-PERSONA-OUTPUT — 未収集の横断投稿者を集中収集する。 */
export async function collectCrossAuthors(
  ports: SteamFetchPorts,
  store: SteamPersonaStore,
  opts: CollectOptions
): Promise<CollectResult> {
  const targets = store.listUncollectedAuthors().slice(0, opts.maxAuthorsPerRun);
  let authors = 0;
  let reviews = 0;
  let failedAuthors = 0;

  for (const author of targets) {
    // Spec §2-4: 非公開プロフィール等の「取得」失敗は 0 件で記録して次へ進む (エラーにしない)。
    // catch は fetch だけに掛ける — 後続の書き出しは SteamID64 検証 (出力パス保護) を含むため、
    // そこでの失敗は取得失敗ではなく不変条件違反として呼び出し元へ伝播させる。
    let entries: UserReviewEntry[];
    try {
      entries = await ports.fetchUserReviews({ steamId: author.steamId });
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      opts.log?.(`author ${author.steamId}: fetch failed (${message}), record 0 reviews`);
      store.markAuthorCollected(author.steamId, opts.now(), 0);
      authors++;
      failedAuthors++;
      continue;
    }

    const at = opts.now();
    const items = entries.map((e) => mapUserReviewToUtterance(e, at));
    const file = writeAuthorJsonl(opts.outDir, author.steamId, items);
    store.markAuthorCollected(author.steamId, at, items.length);
    reviews += items.length;
    authors++;
    opts.log?.(`author ${author.steamId}: ${items.length} reviews -> ${file}`);
  }

  return { authors, reviews, failedAuthors };
}
