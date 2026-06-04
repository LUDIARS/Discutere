/**
 * External discussion sources — 取り込み中間データモデル (Phase 1).
 *
 * 外部の公開ゲーム議論 (Steam レビュー / YouTube コメント等) を収集し、
 * Discatier Core の utterances / reactions に格納するための正規化済み 1 発話。
 * 仕様は spec/crawler/EXTERNAL-SOURCES.md。
 */

export type ExternalSource = "steam" | "youtube" | "reddit" | "niconico" | "fandom";

/** 賛否 / 人気度 (取得元にあれば) */
export interface ExternalSignal {
  /** Steam recommended 等。賛否が二値で取れる source のみ */
  votedUp?: boolean;
  /** いいね / upvote / helpful 数 */
  upvotes?: number;
}

/** collector の出力かつ JSONL stage の 1 行 */
export interface ExternalUtterance {
  source: ExternalSource;
  /** 取得元での一意 id (dedup key の素) */
  nativeId: string;
  /** どのゲームの議論か (Game.id と揃える slug) */
  gameSlug: string;
  /** 議論の場の単位 → session_id に対応 (appid / 動画id / スレッドid) */
  threadKey: string;
  /** 発話本文 (原文) */
  content: string;
  /** "ja" | "en" 等 (判明すれば) */
  lang?: string;
  /** epoch ms */
  postedAt: number;
  /**
   * 公開・安定な同一性 ID (SteamID64 / channelId / username 等)。
   * persona の判定アンカーとして保持する (spec §6「情報精度 > プライバシー」)。
   */
  authorId: string;
  /** 公開表示名 (persona の手触り。任意。逆引き用途には使わない) */
  authorName?: string;
  /** 返信元 nativeId (スレッド構造) → responds_to に対応 */
  parentNativeId?: string;
  signal?: ExternalSignal;
  /** canonical な引用元 (監査用) */
  sourceUrl: string;
}
