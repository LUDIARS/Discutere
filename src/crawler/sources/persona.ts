/**
 * 話者の同一性 (persona アンカー) と露出マスク — spec/crawler/EXTERNAL-SOURCES.md §6.
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
