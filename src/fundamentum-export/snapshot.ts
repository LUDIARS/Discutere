/**
 * 議論 (ConclusionDetail) を Fundamentum master に put できる正規化 JSON スナップショットへ変換する。
 * SRP: 純関数の変換のみ (I/O なし)。
 */
import type { JsonValue } from "fundamentum";
import type { ConclusionDetail } from "../visualize/conclusions.js";

/**
 * JSON.parse(JSON.stringify(...)) で undefined を落とし、JsonValue と構造的に一致させる。
 *
 * 注意: 「いつ export したか」は含めない。master は content-addressed (同じ内容 → 同じ id) なので、
 * 議論が変わっていなければ再 export しても id/catalog version が変わらない (= dedup) のが正しい挙動。
 * exportedAt を混ぜると毎回別 content id になり dedup が壊れる。
 */
export function buildDiscussionSnapshot(detail: ConclusionDetail): JsonValue {
  const raw = {
    discussionId: detail.gapId,
    kind: detail.kind,
    title: detail.title,
    conclusion: detail.conclusion,
    paper: detail.paper,
    aufhebungen: detail.aufhebungen,
    topOpinions: detail.topOpinions,
    transcript: detail.transcript,
    utteranceCount: detail.utteranceCount,
    aufhebungCount: detail.aufhebungCount,
    updatedAt: detail.updatedAt,
  };
  return JSON.parse(JSON.stringify(raw)) as JsonValue;
}
