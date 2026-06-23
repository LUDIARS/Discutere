/**
 * 話者の同一性 (persona アンカー) と露出マスク — spec/feature/crawler/EXTERNAL-SOURCES.md §6.
 *
 * 統治原則「情報精度 > プライバシー」を二層で実装する:
 *  - 保管層: 公開・安定 ID をそのまま speaker_id にする (toSpeakerId)。persona 精度を保つ。
 *  - 露出層: 議論ユーザに見せる時は不可逆なペルソナ表示名に置換する (maskedPersonaLabel)。
 *    個人特定情報 (公開 ID) は end user に渡さない。
 */

import { createHash } from "node:crypto";

import type { ExternalSource } from "./types.js";

/** 保管層: 公開・安定 ID を speaker_id アンカー化する (`ext:<source>:<authorId>`)。 */
export function toSpeakerId(source: ExternalSource, authorId: string): string {
  return `ext:${source}:${authorId}`;
}

/**
 * 露出層: 議論ユーザに見せる不可逆なペルソナ表示名。
 * speaker_id (公開 ID を含む) を一方向ハッシュし「論者#xxxxxx」へ変換する。
 * これ単体から公開 ID は復元できない (= end user からの逆引き不可)。
 * 同一 speaker_id は常に同じ表示名 = 議論上の人物の一貫性は保てる。
 */
export function maskedPersonaLabel(speakerId: string): string {
  const digest = createHash("sha256").update(speakerId).digest("hex").slice(0, 6);
  return `論者#${digest}`;
}

/** 出所メタの最小形 (attribution-store の Attribution と互換)。露出時に必要な分だけ。 */
export interface SourceAttribution {
  source: string;
  sourceUrl: string;
}

/** 出所バッジ (§6 — ソース種別 + 元 URL を end user に開示)。 attribution 無しなら null。 */
export function sourceBadge(attribution: SourceAttribution | null | undefined): { source: string; url: string } | null {
  if (!attribution) return null;
  return { source: attribution.source, url: attribution.sourceUrl };
}

/**
 * 露出層の二層シリアライズ (§6「出所は透明 / 個人は仮名」)。
 * 個人 (speaker_id = 公開 ID アンカー) はペルソナ名へマスクし、 出所 (source / sourceUrl) は
 * attribution があれば素通しで開示する。 attribution が無い (内部 Discord 発話) なら出所は null。
 */
export function maskedSpeakerWithSource(
  speakerId: string,
  attribution: SourceAttribution | null | undefined
): { speaker: string; source: string | null; sourceUrl: string | null } {
  const badge = sourceBadge(attribution);
  return {
    speaker: maskedPersonaLabel(speakerId),
    source: badge?.source ?? null,
    sourceUrl: badge?.url ?? null,
  };
}
