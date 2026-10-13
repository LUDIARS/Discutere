/**
 * Voluptas が届ける「遊んだ感想」の受け取り (spec/feature/flow/voluptas-impression-push.md)。
 *
 * Voluptas には匿名性を担保したうえで Discutere に情報を提供する義務がある (neco 2026-10-13)。
 * Voluptas が自動で送ってくる感想を検査し、ユーザーの声 (source=glab) として KG に取り込む。
 *
 * 匿名性は送り手 (Voluptas の listImpressions) で担保し、受け手でも次を確かめる:
 *   - 1 件が持てるのは id / gameTitle / recommend / polarity / comment / createdAt だけ。
 *     それ以外 (userId / authorId / name 等) を含む感想は 1 件ごと拒否して数だけ返す
 *   - 書き手は常に匿名 (GLAB_ANONYMOUS_AUTHOR) で保存する (mapImpression)
 * 取込は既存の importer (dedup あり) なので、同じ感想を何度送られても二重にならない。
 */

import type { ExternalUtterance } from "../crawler/sources/types.js";
import { glabGameSlug, utteranceIdOf } from "../flow/user-voices/collect.js";
import { mapImpression, type VoluptasImpression } from "../flow/user-voices/voluptas-impressions.js";

/** 1 回の送信で受ける最大件数。 */
export const IMPRESSION_PUSH_MAX = 200;

/** 匿名の感想が持てる項目。 */
const ALLOWED_KEYS = new Set(["id", "gameTitle", "recommend", "polarity", "comment", "createdAt"]);

export type SkipReason = "non_anonymous_field" | "invalid";

export interface ParsedImpressionBatch {
  impressions: VoluptasImpression[];
  skipped: Record<SkipReason, number>;
}

export class ImpressionBatchError extends Error {}

function isAnonymous(o: Record<string, unknown>): boolean {
  return Object.keys(o).every((k) => ALLOWED_KEYS.has(k));
}

function isValid(o: Record<string, unknown>): boolean {
  if (typeof o.id !== "string" || !/^[\w.:-]{1,128}$/.test(o.id)) return false;
  if (typeof o.gameTitle !== "string" || !o.gameTitle.trim() || o.gameTitle.length > 200) return false;
  if (typeof o.comment !== "string" || !o.comment.trim() || o.comment.length > 10_000) return false;
  if (typeof o.createdAt !== "string" || !Number.isFinite(Date.parse(o.createdAt))) return false;
  if (o.recommend !== undefined && o.recommend !== null && typeof o.recommend !== "boolean") return false;
  if (o.polarity !== undefined && o.polarity !== null && typeof o.polarity !== "string") return false;
  return true;
}

/** 送信本文 `{ impressions: [...] }` を検査する。形が壊れていれば ImpressionBatchError。 */
export function parseImpressionBatch(body: unknown): ParsedImpressionBatch {
  const list = (body as { impressions?: unknown } | null)?.impressions;
  if (!Array.isArray(list)) throw new ImpressionBatchError("impressions must be an array");
  if (list.length > IMPRESSION_PUSH_MAX) {
    throw new ImpressionBatchError(`impressions must be at most ${IMPRESSION_PUSH_MAX} items`);
  }
  const skipped: Record<SkipReason, number> = { non_anonymous_field: 0, invalid: 0 };
  const impressions: VoluptasImpression[] = [];
  for (const item of list) {
    if (!item || typeof item !== "object" || Array.isArray(item)) {
      skipped.invalid += 1;
      continue;
    }
    const o = item as Record<string, unknown>;
    if (!isAnonymous(o)) {
      skipped.non_anonymous_field += 1;
      continue;
    }
    if (!isValid(o)) {
      skipped.invalid += 1;
      continue;
    }
    impressions.push({
      id: o.id as string,
      gameTitle: (o.gameTitle as string).trim(),
      recommend: (o.recommend as boolean | null | undefined) ?? null,
      polarity: (o.polarity as string | null | undefined) ?? null,
      comment: o.comment as string,
      createdAt: o.createdAt as string,
    });
  }
  return { impressions, skipped };
}

export interface IngestDeps {
  /** 既存 importer (dedup 付き)。@returns 新しく取り込んだ件数。 */
  importItems: (items: ExternalUtterance[]) => number;
  /** 埋め込み (未設定なら undefined)。@returns 書き込んだ件数。 */
  embed?: (utteranceIds: string[]) => Promise<number>;
  warn?: (msg: string) => void;
}

export interface IngestResult {
  received: number;
  imported: number;
  embedded: number;
  /** ゲームごとの件数 (gameSlug → 件数)。 */
  games: Record<string, number>;
}

/** 感想を KG に取り込む (ゲーム単位の slug を付ける)。 */
export async function ingestImpressions(impressions: readonly VoluptasImpression[], deps: IngestDeps): Promise<IngestResult> {
  const items = impressions.map((i) => mapImpression(glabGameSlug(i.gameTitle), i));
  const games: Record<string, number> = {};
  for (const item of items) games[item.gameSlug ?? ""] = (games[item.gameSlug ?? ""] ?? 0) + 1;
  if (items.length === 0) return { received: 0, imported: 0, embedded: 0, games };
  const imported = deps.importItems(items);
  let embedded = 0;
  if (deps.embed) {
    try {
      embedded = await deps.embed(items.map(utteranceIdOf));
    } catch (e) {
      deps.warn?.(`届いた感想のベクトル化に失敗 (キーワード検索で継続): ${(e as Error).message}`);
    }
  }
  return { received: items.length, imported, embedded, games };
}
