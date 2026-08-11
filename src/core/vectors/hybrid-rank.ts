/**
 * ハイブリッド検索の順位融合・多様化 (純関数)。
 *
 * 外部の声 RAG の候補ランキングをアルゴリズムで底上げする 3 部品:
 *   1. rrfFuse — キーワード順位と埋め込み類似順位を Reciprocal Rank Fusion で融合。
 *      スコアのスケール差 (LIKE ヒット数 vs cosine) を順位に落として吸収する。
 *   2. mmrSelect — Maximal Marginal Relevance。関連度と「既選択との非類似」を
 *      両立させ、ほぼ同文の声で prompt 枠が埋まるのを防ぐ。
 *   3. dedupeNearDuplicates — ベクトルが無い声も含めた近似重複抑制
 *      (文字 bigram Jaccard、依存ゼロ)。コピペ拡散したコメントを 1 件に畳む。
 *
 * DB / HTTP に触れない。単体テスト可能。
 */

import { cosineSimilarity } from "./vector-math.js";

/** RRF の順位平滑化定数 (Cormack et al. の慣例値)。 */
const RRF_K = 60;

/**
 * Reciprocal Rank Fusion。複数のランキング (id 配列、先頭=1位) を融合し、
 * id → 融合スコアの Map を返す。片方にしか出ない id もそのまま加点される。
 * @param rankings 各検索系の順位付き id リスト。
 * @param weights 任意の系別重み (省略時 1.0)。rankings と同じ長さ。
 * @implements SPEC-VOICE-RAG-HYBRID-SEARCH
 */
export function rrfFuse(
  rankings: readonly (readonly string[])[],
  weights?: readonly number[]
): Map<string, number> {
  const fused = new Map<string, number>();
  rankings.forEach((ranking, sys) => {
    const w = weights?.[sys] ?? 1.0;
    ranking.forEach((id, i) => {
      fused.set(id, (fused.get(id) ?? 0) + w / (RRF_K + i + 1));
    });
  });
  return fused;
}

export interface MmrCandidate {
  id: string;
  /** 融合済み関連度スコア (大きいほど関連)。 */
  relevance: number;
  /** 埋め込みベクトル。無い候補は多様性ペナルティ無しで relevance により競合する。 */
  vector?: readonly number[];
}

/**
 * Maximal Marginal Relevance で上位 k 件を選ぶ。
 * score = λ * relevance - (1-λ) * max(既選択との cosine 類似)。
 * ベクトルを持たない候補も relevance で選択対象に残し、部分索引で上位候補が落ちるのを防ぐ。
 * @param lambda 関連度と多様性の配分 (既定 0.7 = 関連度寄り)。
 * @implements SPEC-VOICE-RAG-HYBRID-SEARCH
 */
export function mmrSelect(
  candidates: readonly MmrCandidate[],
  k: number,
  lambda = 0.7
): MmrCandidate[] {
  if (k <= 0) return [];
  const remaining = [...candidates].sort((a, b) => b.relevance - a.relevance);
  const selected: MmrCandidate[] = [];
  while (selected.length < k && remaining.length > 0) {
    let bestIdx = 0;
    let bestScore = -Infinity;
    for (let i = 0; i < remaining.length; i += 1) {
      const c = remaining[i];
      let maxSim = 0;
      for (const s of selected) {
        if (!c.vector || !s.vector) continue;
        const sim = cosineSimilarity(c.vector, s.vector);
        if (sim > maxSim) maxSim = sim;
      }
      const score = lambda * c.relevance - (1 - lambda) * maxSim;
      if (score > bestScore) {
        bestScore = score;
        bestIdx = i;
      }
    }
    selected.push(remaining.splice(bestIdx, 1)[0]);
  }
  return selected;
}

/** 文字 bigram の集合 (正規化: 小文字化 + 空白圧縮)。 */
function bigrams(text: string): Set<string> {
  const t = text.toLowerCase().replace(/\s+/g, " ").trim();
  const out = new Set<string>();
  for (let i = 0; i < t.length - 1; i += 1) out.add(t.slice(i, i + 2));
  return out;
}

function jaccard(a: Set<string>, b: Set<string>): number {
  if (a.size === 0 || b.size === 0) return 0;
  let inter = 0;
  const [small, large] = a.size <= b.size ? [a, b] : [b, a];
  for (const g of small) if (large.has(g)) inter += 1;
  return inter / (a.size + b.size - inter);
}

export interface DedupeItem {
  id: string;
  content: string;
}

/**
 * 近似重複する声を先勝ちで畳む (入力順を優先度とみなす = スコア降順で渡す)。
 * コピペ転載・定型文の繰り返しが prompt 枠を食うのを防ぐ。
 * @param threshold bigram Jaccard 類似度の重複判定閾値 (既定 0.82)。
 * @implements SPEC-VOICE-RAG-HYBRID-SEARCH
 */
export function dedupeNearDuplicates<T extends DedupeItem>(
  items: readonly T[],
  threshold = 0.82
): T[] {
  const kept: Array<{ item: T; grams: Set<string> }> = [];
  for (const item of items) {
    const grams = bigrams(item.content);
    const dup = kept.some((k) => jaccard(k.grams, grams) >= threshold);
    if (!dup) kept.push({ item, grams });
  }
  return kept.map((k) => k.item);
}
