/**
 * 公開レビュー (ExternalUtterance JSONL) を 話者 × ジャンル に束ねてパックを作る (S5)。
 *
 * 生の外部 ID (authorId) はここで HMAC の仮名 speakerKey に変え、以降に持ち出さない。
 * 推薦可否 (signal.votedUp) が無いレビュー・短すぎるレビュー・辞書に無いゲームは使わない。
 */

import { createHmac } from "node:crypto";
import type { ExternalUtterance } from "../../../crawler/sources/types.js";
import type { PersonaPack } from "../contract.js";
import { buildPersonaPack, type FormReview, type FormThresholds, type HeldOutText } from "./build-pack.js";
import { gameTitleOf, genresOf, type GenreDictionary } from "./genres.js";

const MIN_REVIEW_CHARS = 20;

/** `ext:<source>:<hmac16>`。鍵が無ければ形成しない (仮名を作れないため)。 */
export function speakerKeyOf(source: string, authorId: string, hmacKey: string): string {
  if (!hmacKey) throw new Error("speaker key の HMAC 鍵がありません");
  const digest = createHmac("sha256", hmacKey).update(`${source}:${authorId}`).digest("hex").slice(0, 16);
  return `ext:${source.toLowerCase().replace(/[^a-z0-9_-]/g, "")}:${digest}`;
}

export interface FormReport {
  speakers: number;
  /** 話者 × ジャンルの組の数 (パックにできたもの)。 */
  packs: number;
  byConfidence: Record<"high" | "medium" | "low", number>;
  skipped: { noSignal: number; tooShort: number; unclassified: number; noAuthor: number };
}

export interface FormPacksArgs {
  utterances: readonly ExternalUtterance[];
  dictionary: GenreDictionary;
  hmacKey: string;
  now: number;
  thresholds?: FormThresholds;
}

export function formPersonaPacks(args: FormPacksArgs): { packs: PersonaPack[]; heldOutTexts: HeldOutText[]; report: FormReport } {
  const skipped = { noSignal: 0, tooShort: 0, unclassified: 0, noAuthor: 0 };
  /** speakerKey → genre → reviews */
  const grouped = new Map<string, Map<string, FormReview[]>>();
  for (const u of args.utterances) {
    if (!u.authorId) {
      skipped.noAuthor++;
      continue;
    }
    const votedUp = u.signal?.votedUp;
    if (typeof votedUp !== "boolean") {
      skipped.noSignal++;
      continue;
    }
    const text = (u.content ?? "").trim();
    if (text.length < MIN_REVIEW_CHARS) {
      skipped.tooShort++;
      continue;
    }
    const genres = genresOf(args.dictionary, u.gameSlug);
    if (!u.gameSlug || genres.length === 0) {
      skipped.unclassified++;
      continue;
    }
    const key = speakerKeyOf(u.source, u.authorId, args.hmacKey);
    const byGenre = grouped.get(key) ?? new Map<string, FormReview[]>();
    grouped.set(key, byGenre);
    for (const genre of genres) {
      const list = byGenre.get(genre) ?? [];
      list.push({ gameSlug: u.gameSlug, gameTitle: gameTitleOf(args.dictionary, u.gameSlug), text, recommended: votedUp, sourceKind: u.source });
      byGenre.set(genre, list);
    }
  }

  const packs: PersonaPack[] = [];
  const heldOutTexts: HeldOutText[] = [];
  const byConfidence = { high: 0, medium: 0, low: 0 };
  for (const [speakerKey, byGenre] of [...grouped.entries()].sort(([a], [b]) => a.localeCompare(b))) {
    for (const [genre, reviews] of [...byGenre.entries()].sort(([a], [b]) => a.localeCompare(b))) {
      const built = buildPersonaPack({
        speakerKey,
        genre,
        genreSpecVersion: args.dictionary.specVersion,
        reviews,
        now: args.now,
        thresholds: args.thresholds,
      });
      if (!built) continue;
      packs.push(built.pack);
      heldOutTexts.push(...built.heldOutTexts);
      byConfidence[built.pack.sufficiency.confidence]++;
    }
  }
  return { packs, heldOutTexts, report: { speakers: grouped.size, packs: packs.length, byConfidence, skipped } };
}
