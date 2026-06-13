/**
 * 20 次元感情ベクトル ユーティリティ (改善フロー design_gap 計算の基盤)。
 *
 * ベクトル空間は `src/crawler/sentiment/` の固定 20 次元 (VECTOR_SPEC) と同一。
 * 正本は `src/crawler/sentiment/analyze.mjs` の `VECTOR_SPEC` /
 * `spec/crawler/SENTIMENT.md`。本モジュールは新しい感情空間を定義せず既存 20 次元を使う
 * (t4-improvement.md の out スコープ準拠)。
 *
 * textToVector は analyze.mjs の features()+buildVector() を「テキスト 1 件」に特化して
 * 移植したもの (n=1 のときと等価)。
 */

import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";

const _require = createRequire(import.meta.url);
const _here = path.dirname(fileURLToPath(import.meta.url));

interface Lexicon {
  polarity: Record<string, number>;
  emotions: Record<string, string[]>;
  aspects: Record<string, string[]>;
  arousal: string[];
}

const LEX = _require(path.join(_here, "../crawler/sentiment/lexicon.json")) as Lexicon;

/** Plutchik 8 感情キー (lexicon.json の emotions 順)。 */
export const EMO_KEYS = Object.keys(LEX.emotions);
/** アスペクト 8 キー (lexicon.json の aspects 順)。 */
export const ASP_KEYS = Object.keys(LEX.aspects);

/**
 * 固定 20 次元の次元名。analyze.mjs の VECTOR_SPEC と同順:
 *   emo.valence, emo.arousal, emo.{8}, asp.{8}, meta.positive_ratio, meta.volume_log
 */
export const VECTOR_DIMS: readonly string[] = [
  "emo.valence",
  "emo.arousal",
  ...EMO_KEYS.map((e) => `emo.${e}`),
  ...ASP_KEYS.map((a) => `asp.${a}`),
  "meta.positive_ratio",
  "meta.volume_log",
];

/** ベクトル次元数 (= 20)。 */
export const DIM = VECTOR_DIMS.length;

const n01 = (v: number): number => Math.max(0, Math.min(1, v));
const r4 = (v: number): number => +v.toFixed(4);

// ── 単一テキスト → 20 次元ベクトル (analyze.mjs features+buildVector の n=1 版) ──

function countHits(text: string, words: string[]): number {
  return words.reduce((s, w) => s + (text.includes(w.toLowerCase()) ? 1 : 0), 0);
}

/**
 * テキスト 1 件を固定 20 次元ベクトル (各次元 0..1) に変換する。
 * 改善フローで「意見を採ったときの感情ベクトル」を求める既定実装。
 */
export function textToVector(text: string): number[] {
  const t = (text || "").toLowerCase();

  // 極性 (valence, -1..1)
  let pol = 0;
  let polHits = 0;
  for (const [w, sc] of Object.entries(LEX.polarity)) {
    if (t.includes(w.toLowerCase())) {
      pol += sc;
      polHits++;
    }
  }
  const valence = polHits ? Math.max(-1, Math.min(1, pol / Math.max(2, polHits))) : 0;

  // Plutchik 8 感情 (出現 0/1)
  const emo = EMO_KEYS.map((e) => (countHits(t, LEX.emotions[e]) > 0 ? 1 : 0));

  // アスペクト 8 (言及 × 極性): 言及あり & 正 → 1 / 言及あり & 負 → 0 / それ以外 → 0.5
  const asp = ASP_KEYS.map((a) => {
    const mentioned = countHits(t, LEX.aspects[a]) > 0;
    if (!mentioned) return 0.5;
    if (valence > 0.05) return 1;
    if (valence < -0.05) return 0;
    return 0.5;
  });

  const arousal = n01((countHits(t, LEX.arousal) + ((text || "").split("!").length - 1)) / 4);
  const positiveRatio = valence > 0.05 ? 1 : 0;
  const volumeLog = n01(Math.log10(1 + 1) / 4); // n=1 件

  return [n01((valence + 1) / 2), arousal, ...emo, ...asp, positiveRatio, volumeLog].map(r4);
}

// ── ベクトル演算 ─────────────────────────────────────────────────────────────

function assertSameDim(a: number[], b: number[]): void {
  if (a.length !== b.length) {
    throw new Error(`vector dim mismatch: ${a.length} vs ${b.length}`);
  }
}

/** 内積。 */
export function dot(a: number[], b: number[]): number {
  assertSameDim(a, b);
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i] * b[i];
  return s;
}

/** a - b (要素差)。 */
export function subtract(a: number[], b: number[]): number[] {
  assertSameDim(a, b);
  return a.map((v, i) => v - b[i]);
}

/** L2 ノルム。 */
export function norm(a: number[]): number {
  return Math.sqrt(dot(a, a));
}

/**
 * a を onto 方向へ射影したときのスカラ射影量 (符号付き) = (a・onto) / |onto|。
 * onto がゼロベクトルなら 0。
 *
 * 改善フローのスコア = 「意見の移動ベクトル」を design_gap 方向へ射影した量。
 * 「近さ (cosine)」ではなく design_gap 方向への移動量を測る (improvement.md 実装メモ)。
 */
export function scalarProjection(a: number[], onto: number[]): number {
  const m = norm(onto);
  if (m === 0) return 0;
  return dot(a, onto) / m;
}

/** cosine 類似度 (-1..1)。どちらかがゼロベクトルなら 0。 */
export function cosine(a: number[], b: number[]): number {
  const na = norm(a);
  const nb = norm(b);
  if (na === 0 || nb === 0) return 0;
  return dot(a, b) / (na * nb);
}
